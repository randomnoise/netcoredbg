// Copyright (c) 2021 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "debugger/evalwaiter.h"
#include "utils/platform.h"

namespace netcoredbg
{

void EvalWaiter::NotifyEvalComplete(ICorDebugThread *pThread, ICorDebugEval *pEval)
{
    std::lock_guard<std::mutex> lock(m_evalResultMutex);
    if (!pThread)
    {
        m_evalResult.reset(nullptr);
        return;
    }

    DWORD threadId = 0;
    pThread->GetID(&threadId);

    std::unique_ptr<evalResultData_t> ppEvalResult(new evalResultData_t);
    if (pEval)
    {
        // CORDBG_S_FUNC_EVAL_HAS_NO_RESULT: Some Func evals will lack a return value, such as those whose return type is void.
        (*ppEvalResult).Status = pEval->GetResult(&((*ppEvalResult).iCorEval));
    }

    if (!m_evalResult || m_evalResult->threadId != threadId)
        return;

    m_evalResult->promiseValue.set_value(std::move(ppEvalResult));
    m_evalResult.reset(nullptr);
}

bool EvalWaiter::IsEvalRunning()
{
    std::lock_guard<std::mutex> lock(m_evalResultMutex);
    return !!m_evalResult;
}

void EvalWaiter::CancelEvalRunning()
{
    std::lock_guard<std::mutex> lock(m_evalResultMutex);

    if (!m_evalResult)
        return;

    ToRelease<ICorDebugEval2> iCorEval2;
    if (SUCCEEDED(m_evalResult->pEval->Abort()) ||
        (SUCCEEDED(m_evalResult->pEval->QueryInterface(IID_ICorDebugEval2, (LPVOID*) &iCorEval2)) &&
         SUCCEEDED(iCorEval2->RudeAbort())))
        m_evalCanceled = true;
}

std::future<std::unique_ptr<EvalWaiter::evalResultData_t> > EvalWaiter::RunEval(
    HRESULT &Status,
    ICorDebugProcess *pProcess,
    ICorDebugThread *pThread,
    ICorDebugEval *pEval,
    WaitEvalResultCallback cbSetupEval)
{
    std::promise<std::unique_ptr<evalResultData_t > > p;
    auto f = p.get_future();
    if (!f.valid())
    {
        LOGE("get_future() returns not valid promise object");
    }

    DWORD threadId = 0;
    pThread->GetID(&threadId);

    std::lock_guard<std::mutex> lock(m_evalResultMutex);
    assert(!m_evalResult); // We can have only 1 eval, and previous must be completed.
    m_evalResult.reset(new evalResult_t{threadId, pEval, std::move(p)});

    // We don't have easy way to abort setuped eval in case of some error in debugger API,
    // try setup eval only if all is OK right before we run process.
    if (FAILED(Status = cbSetupEval(pEval)))
    {
        LOGE("Setup eval failed, %0x", Status);
        m_evalResult.reset(nullptr);
    }
    else if (FAILED(Status = pProcess->Continue(0)))
    {
        LOGE("Continue() failed, %0x", Status);
        m_evalResult.reset(nullptr);
    }

    return f;
}

ICorDebugEval *EvalWaiter::FindEvalForThread(ICorDebugThread *pThread)
{
    std::lock_guard<std::mutex> lock(m_evalResultMutex);

    DWORD threadId = 0;
    if (FAILED(pThread->GetID(&threadId)) || !m_evalResult)
        return nullptr;

    return m_evalResult->threadId == threadId ? m_evalResult->pEval : nullptr;
}

HRESULT EvalWaiter::WaitEvalResult(ICorDebugThread *pThread,
                                  ICorDebugValue **ppEvalResult,
                                  WaitEvalResultCallback cbSetupEval)
{
    // Important! Evaluation should be proceed only for 1 thread.
    std::lock_guard<std::mutex> lock(m_waitEvalResultMutex);

    // During evaluation could be implicitly executing user code, that could provoke callback calls like - breakpoints, exceptions, etc.
    // Make sure, that all managed callbacks ignore standard logic during evaluation and don't pause/interrupt managed code execution.

    HRESULT Status;
    ToRelease<ICorDebugProcess> iCorProcess;
    IfFailRet(pThread->GetProcess(&iCorProcess));
    if (!iCorProcess)
        return E_FAIL;
    DWORD evalThreadId = 0;
    IfFailRet(pThread->GetID(&evalThreadId));

    // Note, we need suspend during eval all managed threads, that not used for eval (delegates, reverse pinvokes, managed threads).
    auto ChangeThreadsState = [&](CorDebugThreadState state)
    {
        ToRelease<ICorDebugThreadEnum> iCorThreadEnum;
        iCorProcess->EnumerateThreads(&iCorThreadEnum);
        ULONG fetched = 0;
        ToRelease<ICorDebugThread> iCorThread;
        while (SUCCEEDED(iCorThreadEnum->Next(1, &iCorThread, &fetched)) && fetched == 1)
        {
            DWORD tid = 0;
            if (SUCCEEDED(iCorThread->GetID(&tid)) && evalThreadId != tid)
            {
                if (FAILED(iCorThread->SetDebugState(state)))
                {
                    if (state == THREAD_SUSPEND)
                        LOGW("%s %s", "SetDebugState(THREAD_SUSPEND) during eval setup failed.",
                            "This may change the state of the process and any breakpoints and exceptions encountered will be skipped.");
                    else
                        LOGW("SetDebugState(THREAD_RUN) during eval failed. Process state was not restored.");
                }
            }
            iCorThread.Free();
        }
    };

    bool evalTimeOut = false;
    auto WaitResult = [&]() -> HRESULT
    {
        ChangeThreadsState(THREAD_SUSPEND);

        ToRelease<ICorDebugEval> iCorEval;
        IfFailRet(pThread->CreateEval(&iCorEval));

        try
        {
            auto f = RunEval(Status, iCorProcess, pThread, iCorEval, cbSetupEval);
            IfFailRet(Status);

            if (!f.valid())
                return E_FAIL;

            // NOTE
            // MSVS 2017 debugger and newer use config file
            // C:\Program Files (x86)\Microsoft Visual Studio\YYYY\VERSION\Common7\IDE\Profiles\CSharp.vssettings
            // by default NormalEvalTimeout is 5000 milliseconds
            //
            // TODO add timeout configuration feature (care about VSCode, MSVS with Tizen plugin, standalone usage)

            std::future_status timeoutStatus = f.wait_for(std::chrono::milliseconds(5000));
            if (timeoutStatus == std::future_status::timeout)
            {
                LOGW("Evaluation timed out.");
                LOGW("%s %s", "To prevent an unsafe abort when evaluating, all threads were allowed to run.",
                     "This may have changed the state of the process and any breakpoints and exceptions encountered have been skipped.");

                // NOTE
                // All CoreCLR releases at least till version 3.1.3, don't have proper x86 implementation for ICorDebugEval::Abort().
                // This issue looks like CoreCLR terminate managed process execution instead of abort evaluation.

                // In this case we have same behaviour as MS vsdbg and MSVS C# debugger - run all managed threads and try to abort eval by any cost.
                // Ignore errors here, this our last chance prevent debugger hangs.
                iCorProcess->Stop(0);
                ChangeThreadsState(THREAD_RUN);

                if (FAILED(iCorEval->Abort()))
                {
                    ToRelease<ICorDebugEval2> iCorEval2;
                    if (SUCCEEDED(iCorEval->QueryInterface(IID_ICorDebugEval2, (LPVOID*) &iCorEval2)))
                        iCorEval2->RudeAbort();
                }

                evalTimeOut = true;
                iCorProcess->Continue(0);
            }
            // Wait for 5 more seconds, give `Abort()` a chance.
            timeoutStatus = f.wait_for(std::chrono::milliseconds(5000));
            if (timeoutStatus == std::future_status::timeout)
            {
                // Looks like can't be aborted, this is fatal error for debugger (debuggee have inconsistent state now).
                iCorProcess->Stop(0);
                m_evalResultMutex.lock();
                m_evalResult.reset(nullptr);
                m_evalResultMutex.unlock();
                LOGE("Fatal error, eval abort failed.");
                return E_UNEXPECTED;
            }

            auto evalResult = f.get();
            IfFailRet(evalResult.get()->Status);

            if (!ppEvalResult)
                return S_OK;

            *ppEvalResult = evalResult.get()->iCorEval.Detach();
            return evalResult.get()->Status;
        }
        catch (const std::future_error&)
        {
            return E_FAIL;
        }
    };

    SetEnableCustomNotification(iCorProcess, TRUE);

    m_evalCanceled = false;
    m_evalCrossThreadDependency = false;
    HRESULT ret = WaitResult();

    SetEnableCustomNotification(iCorProcess, FALSE);

    if (ret == CORDBG_S_FUNC_EVAL_ABORTED)
    {
        if (m_evalCrossThreadDependency)
            ret = CORDBG_E_CANT_CALL_ON_THIS_THREAD;
        else
            ret = m_evalCanceled ? COR_E_OPERATIONCANCELED : COR_E_TIMEOUT;
    }
    // In this case we have same behaviour as MS vsdbg and MSVS C# debugger - in case it was aborted with timeout, show proper error.
    else if (evalTimeOut)
    {
        ret = (ret == E_UNEXPECTED) ? E_UNEXPECTED : COR_E_TIMEOUT;
    }

    ChangeThreadsState(THREAD_RUN);
    return ret;
}

HRESULT EvalWaiter::ManagedCallbackCustomNotification(ICorDebugThread *pThread)
{
    // NOTE
    // All CoreCLR releases at least till version 3.1.3, don't have proper x86 implementation for ICorDebugEval::Abort().
    // This issue looks like CoreCLR terminate managed process execution instead of abort evaluation.

    // Note, could by only one eval running, but we need ignore custom notification from threads created during eval.
    // In this case we have same behaviour as MSVS C# debugger (ATM vsdbg don't support Debugger.NotifyOfCrossThreadDependency).
    ICorDebugEval *pEval = FindEvalForThread(pThread);
    if (pEval == nullptr)
        return S_OK;

    HRESULT Status;
    ToRelease<ICorDebugEval2> iCorEval2;
    if (FAILED(Status = pEval->Abort()) &&
        (FAILED(Status = pEval->QueryInterface(IID_ICorDebugEval2, (LPVOID*) &iCorEval2)) ||
         FAILED(Status = iCorEval2->RudeAbort())))
    {
        LOGE("Can't abort evaluation in custom notification callback, %0x", Status);
        return Status;
    }

    m_evalCrossThreadDependency = true;
    return S_OK;
}

HRESULT EvalWaiter::SetupCrossThreadDependencyNotificationClass(ICorDebugModule *pModule)
{
    HRESULT Status;
    ToRelease<IUnknown> pMDUnknown;
    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
    ToRelease<IMetaDataImport> pMD;
    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMD));

    // in order to make code simple and clear, we don't check enclosing classes with recursion here
    // since we know behaviour for sure, just find "System.Diagnostics.Debugger" first
    mdTypeDef typeDefParent = mdTypeDefNil;
    static const WCHAR strParentTypeDef[] = W("System.Diagnostics.Debugger");
    IfFailRet(pMD->FindTypeDefByName(strParentTypeDef, mdTypeDefNil, &typeDefParent));

    mdTypeDef typeDef = mdTypeDefNil;
    static const WCHAR strTypeDef[] = W("CrossThreadDependencyNotification");
    IfFailRet(pMD->FindTypeDefByName(strTypeDef, typeDefParent, &typeDef));

    m_iCorCrossThreadDependencyNotification.Free(); // allow re-setup if need
    return pModule->GetClassFromToken(typeDef, &m_iCorCrossThreadDependencyNotification);
}

HRESULT EvalWaiter::SetEnableCustomNotification(ICorDebugProcess *pProcess, BOOL fEnable)
{
    HRESULT Status;
    ToRelease<ICorDebugProcess3> pProcess3;
    IfFailRet(pProcess->QueryInterface(IID_ICorDebugProcess3, (LPVOID*) &pProcess3));
    return pProcess3->SetEnableCustomNotification(m_iCorCrossThreadDependencyNotification, fEnable);
}

} // namespace netcoredbg
