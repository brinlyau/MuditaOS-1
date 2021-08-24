﻿// Copyright (c) 2017-2021, Mudita Sp. z.o.o. All rights reserved.
// For licensing, see https://github.com/mudita/MuditaOS/LICENSE.md

#include "SystemManagerCommon.hpp"

#include "DependencyGraph.hpp"
#include "graph/TopologicalSort.hpp"

#include <EventStore.hpp>
#include "thread.hpp"
#include "ticks.hpp"
#include "critical.hpp"
#include <algorithm>
#include <service-evtmgr/KbdMessage.hpp>
#include <service-evtmgr/BatteryMessages.hpp>
#include <service-evtmgr/Constants.hpp>
#include <service-evtmgr/EventManagerServiceAPI.hpp>
#include <service-evtmgr/EVMessages.hpp>
#include <service-appmgr/messages/UserPowerDownRequest.hpp>
#include <service-desktop/service-desktop/Constants.hpp>
#include <service-cellular/CellularServiceAPI.hpp>
#include <service-cellular/CellularMessage.hpp>
#include <service-appmgr/Constants.hpp>
#include <service-appmgr/Controller.hpp>
#include "messages/CpuFrequencyMessage.hpp"
#include "messages/DeviceRegistrationMessage.hpp"
#include "messages/SentinelRegistrationMessage.hpp"
#include "messages/RequestCpuFrequencyMessage.hpp"
#include "messages/PhoneModeRequest.hpp"
#include "messages/TetheringStateRequest.hpp"
#include "messages/TetheringQuestionRequest.hpp"
#include "messages/TetheringPhoneModeChangeProhibitedMessage.hpp"
#include <time/ScopedTime.hpp>
#include "Timers/TimerFactory.hpp"
#include <service-appmgr/StartupType.hpp>
#include <purefs/vfs_subsystem.hpp>
#include <service-gui/Common.hpp>
#include <service-db/DBServiceName.hpp>
#include <module-gui/gui/Common.hpp>
#include <service-eink/Common.hpp>

const inline size_t systemManagerStack = 4096 * 2;

namespace sys
{
    namespace
    {
        const std::map<bsp::KeyCodes, phone_modes::PhoneMode> SliderStateToPhoneModeMapping = {
            {bsp::KeyCodes::SSwitchUp, phone_modes::PhoneMode::Connected},
            {bsp::KeyCodes::SSwitchMid, phone_modes::PhoneMode::DoNotDisturb},
            {bsp::KeyCodes::SSwitchDown, phone_modes::PhoneMode::Offline}};

        constexpr std::chrono::milliseconds preShutdownRoutineTimeout{1500};
        constexpr std::chrono::milliseconds lowBatteryShutdownDelayTime{5000};
    } // namespace

    namespace state
    {
        namespace update
        {
            static constexpr std::array whitelist = {service::name::service_desktop,
                                                     service::name::evt_manager,
                                                     service::name::gui,
                                                     service::name::db,
                                                     service::name::eink,
                                                     service::name::appmgr};
        }

        namespace restore
        {
            static constexpr std::array whitelist = {service::name::service_desktop,
                                                     service::name::evt_manager,
                                                     service::name::gui,
                                                     service::name::eink,
                                                     service::name::appmgr};
        }

        namespace regularClose
        {
            static constexpr std::array whitelist = {service::name::evt_manager};
        }

        template <typename T> static bool isOnWhitelist(const T &list, const std::string &serviceName)
        {
            return std::find(std::begin(list), std::end(list), serviceName) != std::end(list);
        }

    } // namespace state

    using namespace cpp_freertos;
    using namespace std;
    using namespace sys;

    void SystemManagerCommon::set(enum State state)
    {
        LOG_DEBUG("System manager state: [%s] -> [%s]", c_str(this->state), c_str(state));
        this->state = state;
    }

    SystemManagerCommon::SystemManagerCommon(std::vector<std::unique_ptr<BaseServiceCreator>> &&creators)
        : Service(service::name::system_manager, "", systemManagerStack), systemServiceCreators{std::move(creators)}
    {
        // Specify list of channels which System Manager is registered to
        bus.channels            = {BusChannel::SystemManagerRequests};
        lowBatteryShutdownDelay = sys::TimerFactory::createPeriodicTimer(
            this, "lowBatteryShutdownDelay", lowBatteryShutdownDelayTime, [this](sys::Timer &) {
                CloseSystemHandler(CloseReason::LowBattery);
            });
    }

    SystemManagerCommon::~SystemManagerCommon()
    {
        LOG_DEBUG("%s", (GetName() + ":destructor").c_str());
    }

    void SystemManagerCommon::Run()
    {
        initialize();

        // in shutdown we need to wait till event manager tells us that it's ok to stfu
        while (state == State::Running) {
            if (auto msg = mailbox.pop(); msg) {
                msg->Execute(this);
            }
        }

        while (state == State::Shutdown) {
            // check if we are discharging - if so -> shutdown
            if (Store::Battery::get().state == Store::Battery::State::Discharging) {
                set(State::ShutdownReady);
            }
            else {
                // await from EvtManager for info that red key was pressed / timeout
                auto msg = mailbox.pop();
                if (!msg) {
                    continue;
                }
                if (msg->sender != service::name::evt_manager) {
                    LOG_ERROR("Ignored msg from: %s on shutdown", msg->sender.c_str());
                    continue;
                }
                msg->Execute(this);
            }
        }

        DestroySystemService(service::name::evt_manager, this);

        CloseService();

        EndScheduler();

        // Power off system
        switch (state) {
        case State::Reboot:
            LOG_INFO("  --->  REBOOT <--- ");
            powerManager->Reboot();
            break;
        case State::ShutdownReady:
            LOG_INFO("  ---> SHUTDOWN <--- ");
            powerManager->PowerOff();
            break;
        case State::RebootToUpdate:
            LOG_INFO("  ---> REBOOT TO UPDATER <--- ");
            powerManager->RebootToUpdate(updateReason);
            break;
        default:
            LOG_FATAL("State changed after reset/shutdown was requested to: %s! this is terrible failure!",
                      c_str(state));
            exit(1);
        }
    }

    void SystemManagerCommon::initialize()
    {
        utils::time::Scoped timer{"Initialize"};
        InitHandler();
        if (systemInit) {
            systemInit();
        }

        StartSystemServices();
        if (userInit) {
            userInit();
        }
    }

    void SystemManagerCommon::StartSystemServices()
    {
        DependencyGraph depGraph{graph::nodesFrom(systemServiceCreators), std::make_unique<graph::TopologicalSort>()};
        const auto &sortedServices = [&depGraph]() {
            utils::time::Scoped timer{"DependencyGraph"};
            return depGraph.sort();
        }();

        LOG_INFO("Order of system services initialization:");
        for (const auto &service : sortedServices) {
            LOG_INFO("\t> %s", service.get().getName().c_str());
        }
        std::for_each(sortedServices.begin(), sortedServices.end(), [this](const auto &service) {
            const auto startTimeout = service.get().getStartTimeout().count();
            if (const auto success = RunSystemService(service.get().create(), this, startTimeout); !success) {
                LOG_FATAL("Unable to start service: %s", service.get().getName().c_str());
                throw SystemInitialisationError{"System startup failed: unable to start a system service."};
            }
        });

        postStartRoutine();
    }

    void SystemManagerCommon::StartSystem(InitFunction sysInit, InitFunction appSpaceInit)
    {
        powerManager     = std::make_unique<PowerManager>();
        cpuStatistics    = std::make_unique<CpuStatistics>();
        deviceManager    = std::make_unique<DeviceManager>();
        phoneModeSubject = std::make_unique<phone_modes::Subject>(this);

        systemInit = std::move(sysInit);
        userInit   = std::move(appSpaceInit);

        // Start System manager
        StartService();

        cpuStatisticsTimer = sys::TimerFactory::createPeriodicTimer(
            this, "cpuStatistics", constants::timerInitInterval, [this](sys::Timer &) { CpuStatisticsTimerHandler(); });
        cpuStatisticsTimer.start();
    }

    bool SystemManagerCommon::CloseSystem(Service *s)
    {
        s->bus.sendUnicast(std::make_shared<SystemManagerCmd>(Code::CloseSystem), service::name::system_manager);
        return true;
    }

    bool SystemManagerCommon::Restore(Service *s)
    {
        LOG_DEBUG("trying to enter restore state");
        auto ret = s->bus.sendUnicastSync(std::make_shared<SystemManagerCmd>(Code::Restore),
                                          service::name::system_manager,
                                          sys::constants::restoreTimeout);
        if (ret.first != ReturnCodes::Success) {
            LOG_WARN("Can't stop all services, %d ms wait time", sys::constants::restoreTimeout);
        }
        return true;
    }

    bool SystemManagerCommon::Reboot(Service *s)
    {
        s->bus.sendUnicast(std::make_shared<SystemManagerCmd>(Code::Reboot), service::name::system_manager);
        return true;
    }

    bool SystemManagerCommon::RebootToUpdate(Service *s, UpdateReason updateReason)
    {
        s->bus.sendUnicast(std::make_shared<SystemManagerCmd>(Code::RebootToUpdate, CloseReason::Reboot, updateReason),
                           service::name::system_manager);
        return true;
    }

    bool SystemManagerCommon::SuspendService(const std::string &name, sys::Service *caller)
    {
        auto ret = caller->bus.sendUnicastSync(
            std::make_shared<SystemMessage>(SystemMessageType::SwitchPowerMode, ServicePowerMode::SuspendToRAM),
            name,
            1000);
        auto resp = std::static_pointer_cast<ResponseMessage>(ret.second);

        if (ret.first != ReturnCodes::Success && (resp->retCode != ReturnCodes::Success)) {
            LOG_FATAL("Service %s failed to enter low-power mode", name.c_str());
        }
        return true;
    }

    bool SystemManagerCommon::ResumeService(const std::string &name, sys::Service *caller)
    {
        auto ret = caller->bus.sendUnicastSync(
            std::make_shared<SystemMessage>(SystemMessageType::SwitchPowerMode, ServicePowerMode::Active), name, 1000);
        auto resp = std::static_pointer_cast<ResponseMessage>(ret.second);

        if (ret.first != ReturnCodes::Success && (resp->retCode != ReturnCodes::Success)) {
            LOG_FATAL("Service %s failed to exit low-power mode", name.c_str());
        }
        return true;
    }

    bool SystemManagerCommon::RunService(std::shared_ptr<Service> service, Service *caller, TickType_t timeout)
    {
        service->StartService();

        auto msg  = std::make_shared<SystemMessage>(SystemMessageType::Start);
        auto ret  = caller->bus.sendUnicastSync(msg, service->GetName(), timeout);
        auto resp = std::static_pointer_cast<ResponseMessage>(ret.second);

        if (ret.first == ReturnCodes::Success && (resp->retCode == ReturnCodes::Success)) {
            return true;
        }
        return false;
    }

    bool SystemManagerCommon::RunSystemService(std::shared_ptr<Service> service, Service *caller, TickType_t timeout)
    {
        CriticalSection::Enter();
        servicesList.push_back(service);
        CriticalSection::Exit();

        return RunService(std::move(service), caller, timeout);
    }

    bool SystemManagerCommon::RunApplication(std::shared_ptr<app::Application> app, Service *caller, TickType_t timeout)
    {
        CriticalSection::Enter();
        applicationsList.push_back(app);
        CriticalSection::Exit();

        return RunService(std::move(app), caller, timeout);
    }

    bool SystemManagerCommon::RequestServiceClose(const std::string &name, Service *caller, TickType_t timeout)
    {
        auto msg  = std::make_shared<SystemMessage>(SystemMessageType::Exit);
        auto ret  = caller->bus.sendUnicastSync(msg, name, timeout);
        auto resp = std::static_pointer_cast<ResponseMessage>(ret.second);

        if (ret.first != ReturnCodes::Success) {
            LOG_ERROR("Service to close: %s did not respond", name.c_str());
            return false;
        }
        else if (resp->retCode != ReturnCodes::Success) {
            LOG_ERROR("Service %s noticed failure at close", name.c_str());
            return false;
        }
        return true;
    }

    template <typename T> void SystemManagerCommon::DestroyServices(const T &whitelist)
    {
        cpp_freertos::LockGuard lck(serviceDestroyMutex);
        for (auto service = servicesList.begin(); service != servicesList.end();) {
            if (sys::state::isOnWhitelist<T>(whitelist, (*service)->GetName())) {
                LOG_DEBUG("Delay closing %s", (*service)->GetName().c_str());
                ++service;
            }
            else {
                if (!RequestServiceClose((*service)->GetName(), this)) {
                    LOG_ERROR("Service %s did not respond -> to kill", (*service)->GetName().c_str());
                    kill(*service);
                }
                service = servicesList.erase(service);
            }
        }
    }

    bool SystemManagerCommon::DestroySystemService(const std::string &name, Service *caller)
    {
        cpp_freertos::LockGuard lck(serviceDestroyMutex);
        if (RequestServiceClose(name, caller)) {
            auto service = std::find_if(servicesList.begin(),
                                        servicesList.end(),
                                        [&name](std::shared_ptr<Service> const &s) { return s->GetName() == name; });
            if (service == servicesList.end()) {
                LOG_ERROR("No such service to destroy in the list: %s", name.c_str());
                return false;
            }
            servicesList.erase(service);
            return true;
        }
        return false;
    }

    bool SystemManagerCommon::DestroyApplication(const std::string &name, Service *caller)
    {
        cpp_freertos::LockGuard lck(appDestroyMutex);
        if (RequestServiceClose(name, caller)) {
            auto app =
                std::find_if(applicationsList.begin(),
                             applicationsList.end(),
                             [&name](std::shared_ptr<app::Application> const &s) { return s->GetName() == name; });
            if (app == applicationsList.end()) {
                LOG_ERROR("No such application to destroy in the list: %s", name.c_str());
                return false;
            }
            applicationsList.erase(app);
            return true;
        }
        return false;
    }

    void SystemManagerCommon::preCloseRoutine(CloseReason closeReason)
    {
        for (const auto &service : servicesList) {
            auto msg = std::make_shared<ServiceCloseReasonMessage>(closeReason);
            bus.sendUnicast(std::move(msg), service->GetName());
            readyForCloseRegister.push_back(service->GetName());
        }

        servicesPreShutdownRoutineTimeout = sys::TimerFactory::createPeriodicTimer(
            this, "servicesPreShutdownRoutine", preShutdownRoutineTimeout, [this](sys::Timer &) { CloseServices(); });
        servicesPreShutdownRoutineTimeout.start();
    }

    void SystemManagerCommon::postStartRoutine()
    {
        connect(sevm::BatteryStateChangeMessage(), [&](Message *) {
            switch (Store::Battery::get().levelState) {
            case Store::Battery::LevelState::Normal:
                batteryNormalLevelAction();
                break;
            case Store::Battery::LevelState::Shutdown:
                batteryShutdownLevelAction();
                break;
            case Store::Battery::LevelState::CriticalCharging:
                batteryCriticalLevelAction(true);
                break;
            case Store::Battery::LevelState::CriticalNotCharging:
                batteryCriticalLevelAction(false);
                break;
            }
            return MessageNone{};
        });
    }

    void SystemManagerCommon::batteryCriticalLevelAction(bool charging)
    {
        LOG_INFO("Battery Critical Level reached!");
        CellularServiceAPI::ChangeModulePowerState(this, cellular::service::State::PowerState::Off);
        auto msg = std::make_shared<CriticalBatteryLevelNotification>(true, charging);
        bus.sendUnicast(std::move(msg), service::name::appmgr);
    }

    void SystemManagerCommon::batteryShutdownLevelAction()
    {
        LOG_INFO("Battery level too low - shutting down the system...");
        CloseSystemHandler(CloseReason::LowBattery);
    }

    void SystemManagerCommon::batteryNormalLevelAction()
    {
        LOG_INFO("Battery level normal.");
        CellularServiceAPI::ChangeModulePowerState(this, cellular::service::State::PowerState::On);
        auto battNormalMsg = std::make_shared<CriticalBatteryLevelNotification>(false);
        bus.sendUnicast(std::move(battNormalMsg), service::name::appmgr);
    }

    void SystemManagerCommon::readyToCloseHandler(Message *msg)
    {
        if (!readyForCloseRegister.empty() && servicesPreShutdownRoutineTimeout.isActive()) {
            auto message = static_cast<ReadyToCloseMessage *>(msg);
            LOG_INFO("ready to close %s", message->sender.c_str());
            readyForCloseRegister.erase(
                std::remove(readyForCloseRegister.begin(), readyForCloseRegister.end(), message->sender),
                readyForCloseRegister.end());

            // All services responded
            if (readyForCloseRegister.empty()) {
                LOG_INFO("All services ready to close.");
                servicesPreShutdownRoutineTimeout.stop();
                CloseServices();
            }
        }
    }

    void SystemManagerCommon::kill(std::shared_ptr<Service> const &toKill)
    {
        auto ret = toKill->DeinitHandler();
        if (ret != sys::ReturnCodes::Success) {
            LOG_DEBUG("deinit handler: %s", c_str(ret));
        }
        toKill->CloseHandler();
    }

    ReturnCodes SystemManagerCommon::InitHandler()
    {
        isReady = true;

        connect(SystemManagerCmd(), [&](Message *msg) {
            if (msg->channel == BusChannel::SystemManagerRequests) {
                auto *data = static_cast<SystemManagerCmd *>(msg);

                switch (data->type) {
                case Code::CloseSystem:
                    CloseSystemHandler(data->closeReason);
                    break;
                case Code::Update:
                    UpdateSystemHandler();
                    break;
                case Code::Restore:
                    RestoreSystemHandler();
                    break;
                case Code::Reboot:
                    RebootHandler(State::Reboot);
                    break;
                case Code::RebootToUpdate:
                    RebootHandler(State::RebootToUpdate, data->updateReason);
                    break;
                case Code::None:
                    break;
                }
            }

            return MessageNone{};
        });

        connect(sevm::BatteryStatusChangeMessage(), [&](Message *) {
            if ((state == State::Shutdown) && (Store::Battery::get().state == Store::Battery::State::Discharging)) {
                set(State::ShutdownReady);
            }
            return MessageNone{};
        });

        connect(sevm::KbdMessage(), [&](Message *) {
            // we are in shutdown mode - we received that there was red key pressed -> we need to reboot
            if (state == State::Shutdown) {
                set(State::Reboot);
            }
            return MessageNone{};
        });

        connect(sevm::BatteryBrownoutMessage(), [&](Message *) {
            LOG_INFO("Battery Brownout voltage level reached! Closing system...");
            CloseSystemHandler(CloseReason::SystemBrownout);
            return MessageNone{};
        });

        connect(CellularCheckIfStartAllowedMessage(), [&](Message *) {
            switch (Store::Battery::get().levelState) {
            case Store::Battery::LevelState::Normal:
                CellularServiceAPI::ChangeModulePowerState(this, cellular::service::State::PowerState::On);
                break;
            case Store::Battery::LevelState::CriticalCharging:
                [[fallthrough]];
            case Store::Battery::LevelState::CriticalNotCharging:
                CellularServiceAPI::ChangeModulePowerState(this, cellular::service::State::PowerState::Off);
                break;
            case Store::Battery::LevelState::Shutdown:
                break;
            }
            return MessageNone{};
        });

        connect(app::UserPowerDownRequest(), [&](Message *) {
            CloseSystemHandler(CloseReason::RegularPowerDown);
            return MessageNone{};
        });

        connect(ReadyToCloseMessage(), [&](Message *msg) {
            readyToCloseHandler(msg);
            return MessageNone{};
        });

        connect(typeid(sys::CpuFrequencyMessage), [this](sys::Message *message) -> sys::MessagePointer {
            auto msg = static_cast<sys::CpuFrequencyMessage *>(message);

            if (msg->getAction() == sys::CpuFrequencyMessage::Action::Increase) {
                powerManager->IncreaseCpuFrequency();
                cpuStatisticsTimer.start();
            }
            else if (msg->getAction() == sys::CpuFrequencyMessage::Action::Decrease) {
                powerManager->DecreaseCpuFrequency();
                cpuStatisticsTimer.start();
            }

            return sys::MessageNone{};
        });

        connect(typeid(sys::DeviceRegistrationMessage), [this](sys::Message *message) -> sys::MessagePointer {
            auto msg = static_cast<sys::DeviceRegistrationMessage *>(message);
            deviceManager->RegisterNewDevice(msg->getDevice());

            return sys::MessageNone{};
        });

        connect(typeid(sys::SentinelRegistrationMessage), [this](sys::Message *message) -> sys::MessagePointer {
            auto msg = static_cast<sys::SentinelRegistrationMessage *>(message);
            powerManager->RegisterNewSentinel(msg->getSentinel());

            return sys::MessageNone{};
        });

        connect(typeid(sys::HoldCpuFrequencyMessage), [this](sys::Message *message) -> sys::MessagePointer {
            auto msg = static_cast<sys::HoldCpuFrequencyMessage *>(message);
            powerManager->SetCpuFrequencyRequest(msg->getName(), msg->getRequest());

            return sys::MessageNone{};
        });

        connect(typeid(sys::ReleaseCpuFrequencyMessage), [this](sys::Message *message) -> sys::MessagePointer {
            auto msg = static_cast<sys::ReleaseCpuFrequencyMessage *>(message);
            powerManager->ResetCpuFrequencyRequest(msg->getName());

            return sys::MessageNone{};
        });

        connect(typeid(PhoneModeRequest), [this](sys::Message *message) -> sys::MessagePointer {
            auto request = static_cast<PhoneModeRequest *>(message);
            return handlePhoneModeRequest(request);
        });

        connect(typeid(TetheringStateRequest), [this](sys::Message *message) -> sys::MessagePointer {
            auto request = static_cast<TetheringStateRequest *>(message);
            return handleTetheringStateRequest(request);
        });

        connect(typeid(app::manager::CheckIfStartAllowedMessage), [this](sys::Message *) -> sys::MessagePointer {
            switch (Store::Battery::get().levelState) {
            case Store::Battery::LevelState::Normal:
                bus.sendUnicast(std::make_unique<app::manager::StartAllowedMessage>(app::manager::StartupType::Regular),
                                service::name::appmgr);
                break;
            case Store::Battery::LevelState::Shutdown:
                if (!lowBatteryShutdownDelay.isActive()) {
                    lowBatteryShutdownDelay.start();
                }
                [[fallthrough]];
            case Store::Battery::LevelState::CriticalNotCharging:
                bus.sendUnicast(
                    std::make_unique<app::manager::StartAllowedMessage>(app::manager::StartupType::LowBattery),
                    service::name::appmgr);
                break;
            case Store::Battery::LevelState::CriticalCharging:
                bus.sendUnicast(
                    std::make_unique<app::manager::StartAllowedMessage>(app::manager::StartupType::LowBatteryCharging),
                    service::name::appmgr);
                break;
            }
            return sys::MessageNone{};
        });

        connect(typeid(TetheringEnabledResponse), [this](sys::Message *message) -> sys::MessagePointer {
            auto response = static_cast<TetheringEnabledResponse *>(message);
            return enableTethering(response);
        });

        deviceManager->RegisterNewDevice(powerManager->getExternalRamDevice());

        cpuSentinel = std::make_shared<sys::CpuSentinel>(
            service::name::system_manager, this, [this](bsp::CpuFrequencyHz newFrequency) {
                UpdateResourcesAfterCpuFrequencyChange(newFrequency);
            });
        powerManager->RegisterNewSentinel(cpuSentinel);

        return ReturnCodes::Success;
    }

    MessagePointer SystemManagerCommon::DataReceivedHandler(DataMessage * /*msg*/, ResponseMessage * /*resp*/)
    {
        return std::make_shared<ResponseMessage>();
    }

    void SystemManagerCommon::CloseSystemHandler(CloseReason closeReason)
    {
        LOG_DEBUG("Invoking closing procedure...");

        // In case if other power down request arrive in the meantime
        lowBatteryShutdownDelay.stop();

        // We are going to remove services in reversed order of creation
        CriticalSection::Enter();
        std::reverse(servicesList.begin(), servicesList.end());
        CriticalSection::Exit();

        preCloseRoutine(closeReason);
    }

    void SystemManagerCommon::CloseServices()
    {
        for (const auto &element : readyForCloseRegister) {
            LOG_INFO("Service: %s did not reported before timeout", element.c_str());
        }
        // All delayed messages will be ignored
        readyForCloseRegister.clear();

        DestroyServices(sys::state::regularClose::whitelist);

        set(State::Shutdown);
    }

    void SystemManagerCommon::RestoreSystemHandler()
    {
        LOG_INFO("Entering restore system state");

        // We are going to remove services in reversed order of creation
        CriticalSection::Enter();
        std::reverse(servicesList.begin(), servicesList.end());
        CriticalSection::Exit();

        DestroyServices(sys::state::restore::whitelist);

        LOG_INFO("entered restore state");
    }

    void SystemManagerCommon::UpdateSystemHandler()
    {
        LOG_DEBUG("Starting system update procedure...");

        // We are going to remove services in reversed order of creation
        CriticalSection::Enter();
        std::reverse(servicesList.begin(), servicesList.end());
        CriticalSection::Exit();

        DestroyServices(sys::state::update::whitelist);
    }

    void SystemManagerCommon::RebootHandler(State state, std::optional<UpdateReason> updateReason)
    {
        CloseSystemHandler(CloseReason::Reboot);
        set(state);
        if (updateReason) {
            this->updateReason = updateReason.value();
        }
    }

    void SystemManagerCommon::CpuStatisticsTimerHandler()
    {
        if (!cpuStatisticsTimerInit) {
            cpuStatisticsTimerInit = true;
            cpuStatisticsTimer.restart(constants::timerPeriodInterval);
        }

        cpuStatistics->Update();
        powerManager->UpdateCpuFrequency(cpuStatistics->GetPercentageCpuLoad());
    }

    phone_modes::PhoneMode SystemManagerCommon::translateSliderState(const RawKey &key)
    {
        const auto code = key.keyCode;
        if (code != bsp::KeyCodes::SSwitchUp && code != bsp::KeyCodes::SSwitchMid &&
            code != bsp::KeyCodes::SSwitchDown) {
            throw std::invalid_argument{"Invalid key code passed."};
        }
        return SliderStateToPhoneModeMapping.at(code);
    }

    MessagePointer SystemManagerCommon::handlePhoneModeRequest(PhoneModeRequest *request)
    {
        LOG_INFO("Phone mode change requested.");
        if (phoneModeSubject->isTetheringEnabled()) {
            LOG_WARN("Changing phone mode when tethering is enabled!");
            // display popup
            bus.sendUnicast(std::make_shared<TetheringPhoneModeChangeProhibitedMessage>(), service::name::appmgr);
            return MessageNone{};
        }
        phoneModeSubject->setPhoneMode(request->getPhoneMode());
        return MessageNone{};
    }

    MessagePointer SystemManagerCommon::handleTetheringStateRequest(TetheringStateRequest *request)
    {
        LOG_INFO("Tethering state change requested");

        if (Store::Battery::get().levelState != Store::Battery::LevelState::Normal) {
            LOG_INFO("Tethering state change refused - battery too low");
            return MessageNone{};
        }

        if (const auto requestedState = request->getTetheringState(); requestedState == phone_modes::Tethering::On) {
            bus.sendUnicast(std::make_shared<TetheringQuestionRequest>(), service::name::appmgr);
        }
        else {
            if (const auto tetheringChanged = phoneModeSubject->setTetheringMode(phone_modes::Tethering::Off);
                !tetheringChanged) {
                bus.sendUnicast(std::make_shared<TetheringQuestionAbort>(), service::name::appmgr);
            }
            else {
                // Turned on, disabling...
                LOG_INFO("Disabling tethering");
                bus.sendUnicast(std::make_shared<sevm::RequestPhoneModeForceUpdate>(), service::name::evt_manager);
            }
        }
        return MessageNone{};
    }

    MessagePointer SystemManagerCommon::enableTethering([[maybe_unused]] TetheringEnabledResponse *response)
    {
        phoneModeSubject->setTetheringMode(phone_modes::Tethering::On);
        return MessageNone{};
    }

    void SystemManagerCommon::UpdateResourcesAfterCpuFrequencyChange(bsp::CpuFrequencyHz newFrequency)
    {
        if (newFrequency == bsp::CpuFrequencyHz::Level_1) {
            purefs::subsystem::disk_mgr()->pm_control(purefs::blkdev::pm_state::suspend);
        }
        else {
            purefs::subsystem::disk_mgr()->pm_control(purefs::blkdev::pm_state::active);
        }
    }

    std::vector<std::shared_ptr<Service>> SystemManagerCommon::servicesList;
    std::vector<std::shared_ptr<app::Application>> SystemManagerCommon::applicationsList;
    cpp_freertos::MutexStandard SystemManagerCommon::serviceDestroyMutex;
    cpp_freertos::MutexStandard SystemManagerCommon::appDestroyMutex;
    std::unique_ptr<PowerManager> SystemManagerCommon::powerManager;
    std::unique_ptr<CpuStatistics> SystemManagerCommon::cpuStatistics;
    std::unique_ptr<DeviceManager> SystemManagerCommon::deviceManager;

} // namespace sys