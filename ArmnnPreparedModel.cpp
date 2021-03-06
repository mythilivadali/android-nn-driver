//
// Copyright © 2017 Arm Ltd. All rights reserved.
// See LICENSE file in the project root for full license information.
//

#define LOG_TAG "ArmnnDriver"

#include "ArmnnPreparedModel.hpp"
#include "Utils.hpp"

#include <boost/format.hpp>
#include <log/log.h>
#include <OperationsUtils.h>

#include <cassert>
#include <cinttypes>

using namespace android;

namespace
{
using namespace armnn_driver;

void NotifyCallbackAndCheck(const ::android::sp<IExecutionCallback>& callback, ErrorStatus errorStatus,
                            std::string callingFunction)
{
    Return<void> returned = callback->notify(errorStatus);
    // This check is required, if the callback fails and it isn't checked it will bring down the service
    if (!returned.isOk())
    {
        ALOGE("ArmnnDriver::%s: hidl callback failed to return properly: %s",
            callingFunction.c_str(), returned.description().c_str());
    }
}

bool ValidateRequestArgument(const RequestArgument& requestArg, const armnn::TensorInfo& tensorInfo)
{
    if (requestArg.dimensions.size() != 0)
    {
        if (requestArg.dimensions.size() != tensorInfo.GetNumDimensions())
        {
            ALOGE("Mismatched dimensions (request argument: %zu, expected: %u)",
                  requestArg.dimensions.size(), tensorInfo.GetNumDimensions());
            return false;
        }

        for (unsigned int d = 0; d < tensorInfo.GetNumDimensions(); ++d)
        {
            if (requestArg.dimensions[d] != tensorInfo.GetShape()[d])
            {
                ALOGE("Mismatched size for dimension %d (request argument: %u, expected %u)",
                    d, requestArg.dimensions[d], tensorInfo.GetShape()[d]);
                return false;
            }
        }
    }

    return true;
}

armnn::Tensor GetTensorForRequestArgument(const RequestArgument& requestArg,
    const armnn::TensorInfo& tensorInfo,
    const std::vector<::android::nn::RunTimePoolInfo>& requestPools)
{
    if (!ValidateRequestArgument(requestArg, tensorInfo))
    {
        return armnn::Tensor();
    }

    return armnn::Tensor(tensorInfo, GetMemoryFromPool(requestArg.location, requestPools));
}

inline std::string BuildTensorName(const char* tensorNamePrefix, std::size_t index)
{
    return tensorNamePrefix + std::to_string(index);
}

}

namespace armnn_driver
{

RequestThread ArmnnPreparedModel::m_RequestThread;

template <typename TensorBindingCollection>
void ArmnnPreparedModel::DumpTensorsIfRequired(char const* tensorNamePrefix,
    const TensorBindingCollection& tensorBindings)
{
    if (!m_RequestInputsAndOutputsDumpDir.empty())
    {
        const std::string requestName = boost::str(boost::format("%1%_%2%.dump") % m_NetworkId % m_RequestCount);
        for (std::size_t i = 0u; i < tensorBindings.size(); ++i)
        {
            DumpTensor(m_RequestInputsAndOutputsDumpDir,
                requestName,
                BuildTensorName(tensorNamePrefix, i),
                tensorBindings[i].second);
        }
    }
}

ArmnnPreparedModel::ArmnnPreparedModel(armnn::NetworkId networkId,
    armnn::IRuntime* runtime,
    const Model& model,
    const std::string& requestInputsAndOutputsDumpDir)
: m_NetworkId(networkId)
, m_Runtime(runtime)
, m_Model(model)
, m_RequestCount(0)
, m_RequestInputsAndOutputsDumpDir(requestInputsAndOutputsDumpDir)
{
}

ArmnnPreparedModel::~ArmnnPreparedModel()
{
    //unload the network associated with this model
    m_Runtime->UnloadNetwork(m_NetworkId);
}

Return<ErrorStatus> ArmnnPreparedModel::execute(const Request& request,
                                                const ::android::sp<IExecutionCallback>& callback)
{
    ALOGV("ArmnnPreparedModel::execute(): %s", GetModelSummary(m_Model).c_str());
    m_RequestCount++;

    if (callback.get() == nullptr) {
        ALOGE("ArmnnPreparedModel::execute invalid callback passed");
        return ErrorStatus::INVALID_ARGUMENT;
    }

    if (!android::nn::validateRequest(request, m_Model))
    {
        NotifyCallbackAndCheck(callback, ErrorStatus::INVALID_ARGUMENT, "ArmnnPreparedModel::execute");
        return ErrorStatus::INVALID_ARGUMENT;
    }

    if (!m_RequestInputsAndOutputsDumpDir.empty())
    {
        ALOGD("Dumping inputs and outputs for request %" PRIuPTR, reinterpret_cast<std::uintptr_t>(callback.get()));
    }

    // allocate the tensors on the heap, as they are passed to the request thread
    auto pInputTensors = std::make_shared<armnn::InputTensors>();
    auto pOutputTensors = std::make_shared<armnn::OutputTensors>();

    // map the memory pool into shared pointers
    // use a shared memory pools vector on the heap, as it is passed to the request thread
    auto pMemPools = std::make_shared<std::vector<android::nn::RunTimePoolInfo>>();
    if (!setRunTimePoolInfosFromHidlMemories(pMemPools.get(), request.pools))
    {
        NotifyCallbackAndCheck(callback, ErrorStatus::GENERAL_FAILURE, "ArmnnPreparedModel::execute");
        return ErrorStatus::GENERAL_FAILURE;
    }

    // add the inputs and outputs with their data
    try
    {
        pInputTensors->reserve(request.inputs.size());
        for (unsigned int i = 0; i < request.inputs.size(); i++)
        {
            const auto& inputArg = request.inputs[i];

            const armnn::TensorInfo inputTensorInfo = m_Runtime->GetInputTensorInfo(m_NetworkId, i);
            const armnn::Tensor inputTensor = GetTensorForRequestArgument(inputArg, inputTensorInfo, *pMemPools);
            if (inputTensor.GetMemoryArea() == nullptr)
            {
                ALOGE("Cannot execute request. Error converting request input %u to tensor", i);
                return ErrorStatus::GENERAL_FAILURE;
            }

            pInputTensors->emplace_back(i, inputTensor);
        }

        pOutputTensors->reserve(request.outputs.size());
        for (unsigned int i = 0; i < request.outputs.size(); i++)
        {
            const auto& outputArg = request.outputs[i];

            const armnn::TensorInfo outputTensorInfo = m_Runtime->GetOutputTensorInfo(m_NetworkId, i);
            const armnn::Tensor outputTensor = GetTensorForRequestArgument(outputArg, outputTensorInfo, *pMemPools);
            if (outputTensor.GetMemoryArea() == nullptr)
            {
                ALOGE("Cannot execute request. Error converting request output %u to tensor", i);
                return ErrorStatus::GENERAL_FAILURE;
            }

            pOutputTensors->emplace_back(i, outputTensor);
        }
    }
    catch (armnn::Exception& e)
    {
        ALOGW("armnn::Exception caught while preparing for EnqueueWorkload: %s", e.what());
        NotifyCallbackAndCheck(callback, ErrorStatus::GENERAL_FAILURE, "ArmnnPreparedModel::execute");
        return ErrorStatus::GENERAL_FAILURE;
    }

    ALOGV("ArmnnPreparedModel::execute(...) before PostMsg");
    // post the request for asynchronous execution
    m_RequestThread.PostMsg(this, pMemPools, pInputTensors, pOutputTensors, callback);
    ALOGV("ArmnnPreparedModel::execute(...) after PostMsg");

    return ErrorStatus::NONE; // successfully queued
}

void ArmnnPreparedModel::ExecuteGraph(std::shared_ptr<std::vector<::android::nn::RunTimePoolInfo>>& pMemPools,
                                      std::shared_ptr<armnn::InputTensors>& pInputTensors,
                                      std::shared_ptr<armnn::OutputTensors>& pOutputTensors,
                                      const ::android::sp<IExecutionCallback>& callback)
{
    ALOGV("ArmnnPreparedModel::ExecuteGraph(...)");

    DumpTensorsIfRequired("Input", *pInputTensors);

    // run it
    try
    {
        m_Runtime->EnqueueWorkload(m_NetworkId, *pInputTensors, *pOutputTensors);
    }
    catch (armnn::Exception& e)
    {
        ALOGW("armnn::Exception caught from EnqueueWorkload: %s", e.what());
        NotifyCallbackAndCheck(callback, ErrorStatus::GENERAL_FAILURE, "ArmnnPreparedModel::ExecuteGraph");
        return;
    }

    DumpTensorsIfRequired("Output", *pOutputTensors);

    // Commit output buffers.
    // Note that we update *all* pools, even if they aren't actually used as outputs -
    // this is simpler and is what the CpuExecutor does.
    for (android::nn::RunTimePoolInfo& pool : *pMemPools)
    {
        pool.update();
    }

    NotifyCallbackAndCheck(callback, ErrorStatus::NONE, "ExecuteGraph");
}

void ArmnnPreparedModel::ExecuteWithDummyInputs()
{
    std::vector<std::vector<char>> storage;
    armnn::InputTensors inputTensors;
    for (unsigned int i = 0; i < m_Model.inputIndexes.size(); i++)
    {
        const armnn::TensorInfo inputTensorInfo = m_Runtime->GetInputTensorInfo(m_NetworkId, i);
        storage.emplace_back(inputTensorInfo.GetNumBytes());
        const armnn::ConstTensor inputTensor(inputTensorInfo, storage.back().data());

        inputTensors.emplace_back(i, inputTensor);
    }

    armnn::OutputTensors outputTensors;
    for (unsigned int i = 0; i < m_Model.outputIndexes.size(); i++)
    {
        const armnn::TensorInfo outputTensorInfo = m_Runtime->GetOutputTensorInfo(m_NetworkId, i);
        storage.emplace_back(outputTensorInfo.GetNumBytes());
        const armnn::Tensor outputTensor(outputTensorInfo, storage.back().data());

        outputTensors.emplace_back(i, outputTensor);
    }

    try
    {
        m_Runtime->EnqueueWorkload(m_NetworkId, inputTensors, outputTensors);
    }
    catch (armnn::Exception& e)
    {
        ALOGW("ExecuteWithDummyInputs: armnn::Exception caught from EnqueueWorkload: %s", e.what());
    }
}

AndroidNnCpuExecutorPreparedModel::AndroidNnCpuExecutorPreparedModel(const Model& model,
    const std::string& requestInputsAndOutputsDumpDir)
: m_Model(model)
, m_RequestInputsAndOutputsDumpDir(requestInputsAndOutputsDumpDir)
, m_RequestCount(0)
{
}

bool AndroidNnCpuExecutorPreparedModel::Initialize()
{
    return setRunTimePoolInfosFromHidlMemories(&m_ModelPoolInfos, m_Model.pools);
}

Return<ErrorStatus> AndroidNnCpuExecutorPreparedModel::execute(const Request& request,
    const ::android::sp<IExecutionCallback>& callback)
{
    m_RequestCount++;
    std::vector<android::nn::RunTimePoolInfo> requestPoolInfos;

    if (!setRunTimePoolInfosFromHidlMemories(&requestPoolInfos, request.pools))
    {
        NotifyCallbackAndCheck(callback, ErrorStatus::GENERAL_FAILURE, "AndroidNnCpuExecutorPreparedModel::execute");
        return ErrorStatus::GENERAL_FAILURE;
    }

    if (!m_RequestInputsAndOutputsDumpDir.empty())
    {
        ALOGD("Dumping inputs and outputs for request %" PRIuPTR, reinterpret_cast<std::uintptr_t>(callback.get()));
    }

    DumpTensorsIfRequired(
        "Input",
        m_Model.inputIndexes,
        request.inputs,
        requestPoolInfos);

    android::nn::CpuExecutor executor;
    const int n = executor.run(m_Model, request, m_ModelPoolInfos, requestPoolInfos);
    ErrorStatus executionStatus =
            n == ANEURALNETWORKS_NO_ERROR ? ErrorStatus::NONE : ErrorStatus::GENERAL_FAILURE;

    DumpTensorsIfRequired(
        "Output",
        m_Model.outputIndexes,
        request.outputs,
        requestPoolInfos);

    NotifyCallbackAndCheck(callback, ErrorStatus::GENERAL_FAILURE, "AndroidNnCpuExecutorPreparedModel::execute");
    return executionStatus;
}

void AndroidNnCpuExecutorPreparedModel::DumpTensorsIfRequired(
    char const* tensorNamePrefix,
    const hidl_vec<uint32_t>& operandIndices,
    const hidl_vec<RequestArgument>& requestArgs,
    const std::vector<android::nn::RunTimePoolInfo>& requestPoolInfos)
{
    if (m_RequestInputsAndOutputsDumpDir.empty())
    {
        return;
    }

    for (std::size_t i = 0; i < requestArgs.size(); ++i)
    {
        const Operand& operand = m_Model.operands[operandIndices[i]];
        const armnn::TensorInfo tensorInfo = GetTensorInfoForOperand(operand);
        const armnn::Tensor tensor = GetTensorForRequestArgument(requestArgs[i], tensorInfo, requestPoolInfos);
        const std::string tensorName = BuildTensorName(tensorNamePrefix, i);
        if (tensor.GetMemoryArea() != nullptr)
        {
            std::string requestName = boost::str(boost::format("%1%_%2%.dump") % this % m_RequestCount);
            DumpTensor(m_RequestInputsAndOutputsDumpDir, requestName, tensorName, tensor);
        }
        else
        {
            ALOGE("Cannot dump tensor %s. An error occurred converting the associated request argument to a tensor.",
                tensorName.c_str());
        }
    }
}

}
