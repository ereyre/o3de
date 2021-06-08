/*
 * All or portions of this file Copyright (c) Amazon.com, Inc. or its affiliates or
 * its licensors.
 *
 * For complete copyright and license terms please see the LICENSE at the root of this
 * distribution (the "License"). All use of this software is governed by the License,
 * or, if provided, by the license below or the license accompanying this file. Do not
 * remove or modify any license notices. This file is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *
 */

#include <Editor/Attribution/AWSCoreAttributionManager.h>
#include <Editor/Attribution/AWSCoreAttributionMetric.h>

#include <AzFramework/IO/LocalFileIO.h>
#include <AzCore/std/smart_ptr/unique_ptr.h>
#include <AzCore/std/smart_ptr/make_shared.h>
#include <AzCore/base.h>
#include <AzCore/Settings/SettingsRegistry.h>
#include <AzCore/Settings/SettingsRegistryImpl.h>
#include <AzCore/Settings/SettingsRegistryMergeUtils.h>
#include <AzCore/Serialization/Json/JsonSystemComponent.h>
#include <AzCore/Serialization/Json/RegistrationContext.h>
#include <AzCore/Component/ComponentBus.h>
#include <AzCore/Jobs/JobManager.h>
#include <AzCore/Jobs/JobManagerBus.h>
#include <AzCore/Jobs/JobContext.h>
#include <AzCore/Utils/Utils.h>
#include <AzCore/UnitTest/TestTypes.h>
#include <AzCore/Component/Entity.h>
#include <AzCore/Module/ModuleManagerBus.h>

#include <TestFramework/AWSCoreFixture.h>


using namespace AWSCore;

namespace AWSAttributionUnitTest
{
    class ModuleDataMock:
        public AZ::ModuleData
    {
    public:
        AZStd::shared_ptr<AZ::Entity> m_entity;
        ModuleDataMock(AZStd::string name)
        {
            m_entity = AZStd::make_shared<AZ::Entity>();
            m_entity->SetName(name);
        }
        virtual ~ModuleDataMock()
        {
            m_entity.reset();
        }

        AZ::DynamicModuleHandle* GetDynamicModuleHandle() const override
        {
            return nullptr;
        }
        /// Get the handle to the module class
        AZ::Module* GetModule() const override
        {
            return nullptr;
        }
        /// Get the entity this module uses as a System Entity
        AZ::Entity* GetEntity() const override
        {
            return m_entity.get();
        }
        /// Get the debug name of the module
        const char* GetDebugName() const override
        {
            return m_entity->GetName().c_str();
        }
    };

    class ModuleManagerRequestBusMock
        : public AZ::ModuleManagerRequestBus::Handler
    {
    public:

        void EnumerateModulesMock(AZ::ModuleManagerRequests::EnumerateModulesCallback perModuleCallback)
        {
            auto data = ModuleDataMock("AWSCore.Editor.dll");
            perModuleCallback(data);
            data = ModuleDataMock("AWSClientAuth.so");
            perModuleCallback(data);
        }

        ModuleManagerRequestBusMock()
        {
            AZ::ModuleManagerRequestBus::Handler::BusConnect();
            ON_CALL(*this, EnumerateModules(testing::_)).WillByDefault(testing::Invoke(this, &ModuleManagerRequestBusMock::EnumerateModulesMock));
        }

        ~ModuleManagerRequestBusMock()
        {
            AZ::ModuleManagerRequestBus::Handler::BusDisconnect();
        }

        MOCK_METHOD1(EnumerateModules, void(AZ::ModuleManagerRequests::EnumerateModulesCallback perModuleCallback));
        MOCK_METHOD3(LoadDynamicModule, AZ::ModuleManagerRequests::LoadModuleOutcome(const char* modulePath, AZ::ModuleInitializationSteps lastStepToPerform, bool maintainReference));
        MOCK_METHOD3(LoadDynamicModules, AZ::ModuleManagerRequests::LoadModulesResult(const AZ::ModuleDescriptorList& modules, AZ::ModuleInitializationSteps lastStepToPerform, bool maintainReferences));
        MOCK_METHOD2(LoadStaticModules, AZ::ModuleManagerRequests::LoadModulesResult(AZ::CreateStaticModulesCallback staticModulesCb, AZ::ModuleInitializationSteps lastStepToPerform));
        MOCK_METHOD1(IsModuleLoaded, bool(const char* modulePath));
    };

    class AWSAttributionManagerMock
        : public AWSAttributionManager
    {
    public:
        using AWSAttributionManager::SubmitMetric;
        using AWSAttributionManager::UpdateMetric;
        using AWSAttributionManager::SetApiEndpointAndRegion;


        AWSAttributionManagerMock()
        {
            ON_CALL(*this, SubmitMetric(testing::_)).WillByDefault(testing::Invoke(this, &AWSAttributionManagerMock::SubmitMetricMock));
        }

        MOCK_METHOD1(SubmitMetric, void(AttributionMetric& metric));

        void SubmitMetricMock(AttributionMetric& metric)
        {
            AZ_UNUSED(metric);
            UpdateLastSend();
        }
    };

    class AttributionManagerTest
        : public AWSCoreFixture
    {
    public:

        virtual ~AttributionManagerTest() = default;

    protected:
        AZStd::shared_ptr<AZ::SerializeContext> m_serializeContext;
        AZStd::unique_ptr<AZ::JsonRegistrationContext> m_registrationContext;
        AZStd::shared_ptr<AZ::SettingsRegistryImpl> m_settingsRegistry;
        AZStd::unique_ptr<AZ::JobContext> m_jobContext;
        AZStd::unique_ptr<AZ::JobCancelGroup> m_jobCancelGroup;
        AZStd::unique_ptr<AZ::JobManager> m_jobManager;
        AZStd::array<char, AZ::IO::MaxPathLength> m_resolvedSettingsPath;
        ModuleManagerRequestBusMock m_moduleManagerRequestBusMock;

        void SetUp() override
        {
            AWSCoreFixture::SetUp();

            char rootPath[AZ_MAX_PATH_LEN];
            AZ::Utils::GetExecutableDirectory(rootPath, AZ_MAX_PATH_LEN);
            m_localFileIO->SetAlias("@user@", AZ_TRAIT_TEST_ROOT_FOLDER);

            m_localFileIO->ResolvePath("@user@/Registry/", m_resolvedSettingsPath.data(), m_resolvedSettingsPath.size());
            AZ::IO::SystemFile::CreateDir(m_resolvedSettingsPath.data());

            m_localFileIO->ResolvePath("@user@/Registry/editor_aws_preferences.setreg", m_resolvedSettingsPath.data(), m_resolvedSettingsPath.size());

            m_serializeContext = AZStd::make_unique<AZ::SerializeContext>();

            AZ::JsonSystemComponent::Reflect(m_registrationContext.get());

            m_settingsRegistry = AZStd::make_unique<AZ::SettingsRegistryImpl>();

            m_settingsRegistry->SetContext(m_serializeContext.get());
            m_settingsRegistry->SetContext(m_registrationContext.get());

            AZ::SettingsRegistry::Register(m_settingsRegistry.get());

            AZ::JobManagerDesc jobManagerDesc;
            AZ::JobManagerThreadDesc threadDesc;

            m_jobManager.reset(aznew AZ::JobManager(jobManagerDesc));
            m_jobCancelGroup.reset(aznew AZ::JobCancelGroup());
            jobManagerDesc.m_workerThreads.push_back(threadDesc);
            m_jobContext.reset(aznew AZ::JobContext(*m_jobManager, *m_jobCancelGroup));
            AZ::JobContext::SetGlobalContext(m_jobContext.get());
        }

        void TearDown() override
        {
            AZ::JobContext::SetGlobalContext(nullptr);
            m_jobContext.reset();
            m_jobCancelGroup.reset();
            m_jobManager.reset();

            AZ::SettingsRegistry::Unregister(m_settingsRegistry.get());

            m_settingsRegistry.reset();
            m_serializeContext.reset();
            m_registrationContext.reset();

            m_localFileIO->ResolvePath("@user@/Registry/", m_resolvedSettingsPath.data(), m_resolvedSettingsPath.size());
            AZ::IO::SystemFile::DeleteDir(m_resolvedSettingsPath.data());

            delete AZ::IO::FileIOBase::GetInstance();
            AZ::IO::FileIOBase::SetInstance(nullptr);

            AWSCoreFixture::TearDown();
        }
    };

    TEST_F(AttributionManagerTest, MetricsSettings_AttributionDisabled_SkipsSend)
    {
        // GIVEN
        AWSAttributionManagerMock manager;
        manager.Init();
       
        CreateFile(m_resolvedSettingsPath.data(), R"({
            "Amazon": {
                "AWS": {
                    "Preferences": {
                        "AWSAttributionEnabled": false,
                        "AWSAttributionDelaySeconds": 30
                    }
                }
            }
        })");

        EXPECT_CALL(manager, SubmitMetric(testing::_)).Times(0);
        EXPECT_CALL(m_moduleManagerRequestBusMock, EnumerateModules(testing::_)).Times(0);

        // WHEN
        manager.MetricCheck();

        // THEN
        m_settingsRegistry->MergeSettingsFile(m_resolvedSettingsPath.data(), AZ::SettingsRegistryInterface::Format::JsonMergePatch, "");
        AZ::u64 timeStamp = 0;
        m_settingsRegistry->Get(timeStamp, "/Amazon/AWS/Preferences/AWSAttributionLastTimeStamp");
        ASSERT_TRUE(timeStamp == 0);

        RemoveFile(m_resolvedSettingsPath.data());
    }

    TEST_F(AttributionManagerTest, AttributionEnabled_NoPreviousTimeStamp_SendSuccess)
    {
        // GIVEN
        AWSAttributionManagerMock manager;
        manager.Init();

        CreateFile(m_resolvedSettingsPath.data(), R"({
            "Amazon": {
                "AWS": {
                    "Preferences": {
                        "AWSAttributionEnabled": true,
                        "AWSAttributionDelaySeconds": 30,
                    }
                }
            }
        })");

        EXPECT_CALL(manager, SubmitMetric(testing::_)).Times(1);
        EXPECT_CALL(m_moduleManagerRequestBusMock, EnumerateModules(testing::_)).Times(1);

        // WHEN
        manager.MetricCheck();

        // THEN
        m_settingsRegistry->MergeSettingsFile(m_resolvedSettingsPath.data(), AZ::SettingsRegistryInterface::Format::JsonMergePatch, "");
        AZ::u64 timeStamp = 0;
        m_settingsRegistry->Get(timeStamp, "/Amazon/AWS/Preferences/AWSAttributionLastTimeStamp");
        ASSERT_TRUE(timeStamp > 0);


        RemoveFile(m_resolvedSettingsPath.data());
    }

    TEST_F(AttributionManagerTest, AttributionEnabled_ValidPreviousTimeStamp_SendSuccess)
    {
        // GIVEN
        AWSAttributionManagerMock manager;
        manager.Init();

        CreateFile(m_resolvedSettingsPath.data(), R"({
            "Amazon": {
                "AWS": {
                    "Preferences": {
                        "AWSAttributionEnabled": true,
                        "AWSAttributionDelaySeconds": 30,
                        "AWSAttributionLastTimeStamp": 629400
                    }
                }
            }
        })");

        EXPECT_CALL(manager, SubmitMetric(testing::_)).Times(1);
        EXPECT_CALL(m_moduleManagerRequestBusMock, EnumerateModules(testing::_)).Times(1);

        // WHEN
        manager.MetricCheck();

        // THEN
        m_settingsRegistry->MergeSettingsFile(m_resolvedSettingsPath.data(), AZ::SettingsRegistryInterface::Format::JsonMergePatch, "");
        AZ::u64 timeStamp = 0;
        m_settingsRegistry->Get(timeStamp, "/Amazon/AWS/Preferences/AWSAttributionLastTimeStamp");
        ASSERT_TRUE(timeStamp > 0);

        RemoveFile(m_resolvedSettingsPath.data());
    }

    TEST_F(AttributionManagerTest, AttributionEnabled_DelayNotSatisfied_SendFail)
    {
        // GIVEN
        AWSAttributionManagerMock manager;
        manager.Init();


        CreateFile(m_resolvedSettingsPath.data(), R"({
            "Amazon": {
                "AWS": {
                    "Preferences": {
                        "AWSAttributionEnabled": true,
                        "AWSAttributionDelaySeconds": 300,
                        "AWSAttributionLastTimeStamp": 0
                    }
                }
            }
        })");

        AZ::u64 delayInSeconds = AZStd::chrono::duration_cast<AZStd::chrono::seconds>(AZStd::chrono::system_clock::now().time_since_epoch()).count();
        ASSERT_TRUE(m_settingsRegistry->Set("/Amazon/AWS/Preferences/AWSAttributionLastTimeStamp", delayInSeconds));

        EXPECT_CALL(manager, SubmitMetric(testing::_)).Times(1);
        EXPECT_CALL(m_moduleManagerRequestBusMock, EnumerateModules(testing::_)).Times(1);

        // WHEN
        manager.MetricCheck();

        // THEN
        m_settingsRegistry->MergeSettingsFile(m_resolvedSettingsPath.data(), AZ::SettingsRegistryInterface::Format::JsonMergePatch, "");
        AZ::u64 timeStamp = 0;
        m_settingsRegistry->Get(timeStamp, "/Amazon/AWS/Preferences/AWSAttributionLastTimeStamp");
        ASSERT_TRUE(timeStamp == delayInSeconds);

        RemoveFile(m_resolvedSettingsPath.data());
    }

    TEST_F(AttributionManagerTest, AttributionEnabledNotFound_SendSuccess)
    {
        // GIVEN
        AWSAttributionManagerMock manager;
        manager.Init();

        CreateFile(m_resolvedSettingsPath.data(), R"({
            "Amazon": {
                "AWS": {
                    "Preferences": {
                    }
                }
            }
        })");

        EXPECT_CALL(manager, SubmitMetric(testing::_)).Times(1);
        EXPECT_CALL(m_moduleManagerRequestBusMock, EnumerateModules(testing::_)).Times(1);

        // WHEN
        manager.MetricCheck();

        // THEN
        m_settingsRegistry->MergeSettingsFile(m_resolvedSettingsPath.data(), AZ::SettingsRegistryInterface::Format::JsonMergePatch, "");
        AZ::u64 timeStamp = 0;
        m_settingsRegistry->Get(timeStamp, "/Amazon/AWS/Preferences/AWSAttributionLastTimeStamp");
        ASSERT_TRUE(timeStamp != 0);

        RemoveFile(m_resolvedSettingsPath.data());
    }

    TEST_F(AttributionManagerTest, SetApiEndpointAndRegion_Success)
    {
        // GIVEN
        AWSAttributionManagerMock manager;
        AWSCore::ServiceAPI::AWSAttributionRequestJob::Config* config = aznew AWSCore::ServiceAPI::AWSAttributionRequestJob::Config();

        // WHEN
        manager.SetApiEndpointAndRegion(config);

        // THEN
        ASSERT_TRUE(config->region  == Aws::Region::US_WEST_2);
        ASSERT_TRUE(config->endpointOverride->find("execute-api.us-west-2.amazonaws.com") != Aws::String::npos);

        delete config;
    }

    TEST_F(AttributionManagerTest, UpdateMetric_Success)
    {
        // GIVEN
        AWSAttributionManagerMock manager;
        AttributionMetric metric;

        AZStd::array<char, AZ::IO::MaxPathLength> engineJsonPath;
        m_localFileIO->ResolvePath("@user@/Registry/engine.json", engineJsonPath.data(), engineJsonPath.size());
        CreateFile(engineJsonPath.data(), R"({"O3DEVersion": "1.0.0.0"})");

        m_localFileIO->ResolvePath("@user@/Registry/", engineJsonPath.data(), engineJsonPath.size());
        m_settingsRegistry->Set(AZ::SettingsRegistryMergeUtils::FilePathKey_EngineRootFolder, engineJsonPath.data());

        EXPECT_CALL(m_moduleManagerRequestBusMock, EnumerateModules(testing::_)).Times(1);

        // WHEN
        manager.UpdateMetric(metric);

        // THEN
        AZStd::string serializedMetricValue = metric.SerializeToJson();
        ASSERT_TRUE(serializedMetricValue.find("\"o3de_version\":\"1.0.0.0\"") != AZStd::string::npos);
        ASSERT_TRUE(serializedMetricValue.find(AZ::GetPlatformName(AZ::g_currentPlatform)) != AZStd::string::npos);
        ASSERT_TRUE(serializedMetricValue.find("AWSCore.Editor") != AZStd::string::npos);
        ASSERT_TRUE(serializedMetricValue.find("AWSClientAuth") != AZStd::string::npos);

        RemoveFile(engineJsonPath.data());
    }

} // namespace AWSCoreUnitTest