#include "pch.h"
#include "HookStub.h"
#include "libDoll.h"
#include "Thread.h"

#include "../Detours/repo/src/detours.h"

extern "C" UINT_PTR DollThreadIsCurrent()
{
    // Avoid hook on GetCurrentThreadId() to cause endless loop
    return (UINT_PTR)(ctx.dollThreads.find(ctx.pRealGetCurrentThreadId()) != ctx.dollThreads.end());
}

extern "C" UINT_PTR DollHookGetCurrent(UINT_PTR* context)
{
    return (UINT_PTR)(ctx.dollHooks.find(*context)->second);
}

extern "C" void DollOnHook(UINT_PTR* context)
{
    // Procedure:
    // register current thread
    // EnterCriticalSection
    // hookOriginalSP = context[1];
    // "Before..." operations
    //    Set context[0] & context[1] based on the reply
    //    If reply is Deny, set hookDenySPOffset & hookDenyReturn

    // Replies are implemented by modifying HookOEP & OriginalSP
    //    Allow: HookOEP to pTrampoline, OriginalSP to pBeforeB
    //    Deny: HookOEP to HookStubOnDeny, OriginalSP to pBeforeB
    //    Terminate: HookOEP to DebugBreak / __fastfail, OriginalSP unchanged (This way the call stack is like called DebugBreak() by hand)

    DollThreadRegisterCurrent();

    EnterCriticalSection(&ctx.lockHook);

    LIBDOLL_HOOK* hook = (LIBDOLL_HOOK*)DollHookGetCurrent(context);
    hook->context = context;
    hook->originalSP = context[1];

    // FIXME: MSG_ONHOOK should not really be sent at here, since TPuppet may send other packets at the same time and cause data corruption
    // Replied ACK is received & processed by TPuppetOnRecv()
    ctx.puppet->send(Puppet::PACKET_MSG_ONHOOK(0));
    ctx.puppet->send(Puppet::PACKET_INTEGER(context[0]));

    ctx.waitingHookOEP = context[0];
    WaitForSingleObject(ctx.hEvtHookVerdict, INFINITE);

    if (hook->verdict == 0)
    {
        // Approved
        context[0] = hook->pTrampoline;
        context[1] = (UINT_PTR)hook->pBeforeB;
    }
    else if (hook->verdict == 1)
    {
        // Rejected
        // Parameters are set by TPuppetOnRecv*()
        context[0] = (UINT_PTR)hook->pBeforeDeny;
        context[1] = (UINT_PTR)hook->pBeforeB; // Comment this line to disable "after" phase on a rejected "before" phase
    }
    else
    {
        // Terminate
        context[0] = (UINT_PTR)DebugBreak;
    }
}

extern "C" void DollOnAfterHook(UINT_PTR* context)
{
    // Procedure:
    // "After..." operations
    //    If prompted to Terminate, overwrite hookOriginalSP with DebugBreak / __fastfail
    //    since it is not possible to do it "the pretty way" // TODO: Revise this sentence
    // context[0] = hookOriginalSP;
    // LeaveCriticalSection
    // unregister current thread

    LIBDOLL_HOOK* hook = (LIBDOLL_HOOK*)DollHookGetCurrent(context);
    hook->context = context;

    // FIXME: MSG_ONHOOK should not really be sent at here, since TPuppet may send other packets at the same time and cause data corruption
    // Replied ACK is received & processed by TPuppetOnRecv()
    ctx.puppet->send(Puppet::PACKET_MSG_ONHOOK(1));
    ctx.puppet->send(Puppet::PACKET_INTEGER(context[0]));

    ctx.waitingHookOEP = context[0];
    WaitForSingleObject(ctx.hEvtHookVerdict, INFINITE);

    if (hook->verdict == 0)
    {
        // Approved / Continue
        context[0] = hook->originalSP;
    }
    else
    {
        // Terminate
        context[0] = (UINT_PTR)DebugBreak;
    }

    LeaveCriticalSection(&ctx.lockHook);

    DollThreadUnregisterCurrent();
}

extern "C" void DollOnEPHook(UINT_PTR* context)
{
    // Unhook first
    DetourTransactionBegin();
    DetourDetach(&ctx.pEP, &HookStubEP);
    DetourTransactionCommit();

    // By now the trampoline is destroyed by Detours, so we need to jump (again) to EP manually
    *context = (UINT_PTR)ctx.pEP;

    // Hooks may exist after returning from WaitForSingleObject()
    DollThreadRegisterCurrent();

    // Wait for approval
    WaitForSingleObject(ctx.hEvtEP, INFINITE);

    // By now the EP event is useless and can be destroyed
    CloseHandle(ctx.hEvtEP);
    ctx.hEvtEP = INVALID_HANDLE_VALUE;

    DollThreadUnregisterCurrent();
}

