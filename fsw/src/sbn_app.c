/******************************************************************************
 ** \file sbn_app.c
 **
 **      Copyright (c) 2004-2006, United States government as represented by the
 **      administrator of the National Aeronautics Space Administration.
 **      All rights reserved. This software(cFE) was created at NASA's Goddard
 **      Space Flight Center pursuant to government contracts.
 **
 **      This software may be used only pursuant to a United States government
 **      sponsored project and the United States government may not be charged
 **      for use thereof.
 **
 ** Purpose:
 **      This file contains source code for the Software Bus Network
 **      Application.
 **
 ** Authors:   J. Wilmot/GSFC Code582
 **            R. McGraw/SSI
 **            C. Knight/ARC Code TI
 ******************************************************************************/

/*
 ** Include Files
 */
#include <fcntl.h>
#include <string.h>

#include "cfe.h"
#include "cfe_sb_msg.h"
#include "cfe_sb.h"
#include "sbn_version.h"
#include "sbn_app.h"
#include "sbn_netif.h"
#include "sbn_msgids.h"
#include "sbn_loader.h"
#include "sbn_cmds.h"
#include "sbn_subs.h"
#include "sbn_main_events.h"
#include "sbn_perfids.h"
#include "sbn_tables.h"
#include "cfe_sb_events.h" /* For event message IDs */
#include "cfe_sb_priv.h" /* For CFE_SB_SendMsgFull */
#include "cfe_es.h" /* PerfLog */

#ifndef SBN_TLM_MID
/* backwards compatability in case you're using a MID generator */
#define SBN_TLM_MID SBN_HK_TLM_MID
#endif /* SBN_TLM_MID */

/** \brief SBN global application data. */
SBN_App_t SBN;

/**
 * SBN uses an inline protocol for maintaining the peer connections. Before a
 * peer is connected, an SBN_ANNOUNCE_MSG message is sent. (This is necessary
 * for connectionless protocols such as UDP.) Once the peer is connected,
 * a heartbeat is sent if no traffic is seen from that peer in the last
 * SBN_HEARTBEAT_SENDTIME seconds. If no traffic (heartbeat or data) is seen
 * in the last SBN_HEARTBEAT_TIMEOUT seconds, the peer is considered to be
 * lost/disconnected.
 */
static void RunProtocol(void)
{
    int PeerIdx = 0, NetIdx = 0;
    OS_time_t current_time;

    /* DEBUG_START(); chatty */

    CFE_ES_PerfLogEntry(SBN_PERF_SEND_ID);

    for(NetIdx = 0; NetIdx < SBN.Hk.NetCount; NetIdx++)
    {
        SBN_NetInterface_t *Net = &SBN.Nets[NetIdx];
        for(PeerIdx = 0; PeerIdx < Net->Status.PeerCount; PeerIdx++)
        {
            SBN_PeerInterface_t *Peer = &Net->Peers[PeerIdx];
            OS_GetLocalTime(&current_time);

            if(Peer->Status.State == SBN_ANNOUNCING)
            {
                if(current_time.seconds
                    - Peer->Status.LastSend.seconds
                        > SBN_ANNOUNCE_TIMEOUT)
                {
                    char AnnounceMsg[SBN_IDENT_LEN];
                    strncpy(AnnounceMsg, SBN_IDENT, SBN_IDENT_LEN);
                    SBN_SendNetMsg(SBN_ANNOUNCE_MSG, SBN_IDENT_LEN,
                        (SBN_Payload_t *)&AnnounceMsg, Peer);
                }/* end if */
                continue;
            }/* end if */
            if(current_time.seconds - Peer->Status.LastRecv.seconds
                    > SBN_HEARTBEAT_TIMEOUT)
            {
                /* lost connection, reset */
                CFE_EVS_SendEvent(SBN_PEER_EID, CFE_EVS_INFORMATION,
                    "peer %d lost connection", PeerIdx);
                SBN_RemoveAllSubsFromPeer(Peer);
                Peer->Status.State = SBN_ANNOUNCING;
                continue;
            }/* end if */
            if(current_time.seconds - Peer->Status.LastSend.seconds
                    > SBN_HEARTBEAT_SENDTIME)
            {
                SBN_SendNetMsg(SBN_HEARTBEAT_MSG, 0, NULL, Peer);
            }/* end if */
        }/* end for */
    }/* end for */

    CFE_ES_PerfLogExit(SBN_PERF_SEND_ID);
}/* end RunProtocol */

/**
 * This function waits for the scheduler (SCH) to wake this code up, so that
 * nothing transpires until the cFE is fully operational.
 *
 * @param[in] iTimeOut The time to wait for the scheduler to notify this code.
 * @return CFE_SUCCESS on success, otherwise an error value.
 */
static int32 WaitForWakeup(int32 iTimeOut)
{
    int32 Status = CFE_SUCCESS;
    CFE_SB_MsgPtr_t Msg = 0;

    /* DEBUG_START(); chatty */

    /* Wait for WakeUp messages from scheduler */
    Status = CFE_SB_RcvMsg(&Msg, SBN.CmdPipe, iTimeOut);

    switch(Status)
    {
        case CFE_SB_NO_MESSAGE:
        case CFE_SB_TIME_OUT:
            Status = CFE_SUCCESS;
            break;
        case CFE_SUCCESS:
            SBN_HandleCommand(Msg);
            break;
        default:
            return Status;
    }/* end switch */

    /* For sbn, we still want to perform cyclic processing
    ** if the WaitForWakeup time out
    ** cyclic processing at timeout rate
    */
    CFE_ES_PerfLogEntry(SBN_PERF_RECV_ID);

    RunProtocol();

#ifndef SBN_RECV_TASK
    SBN_RecvNetMsgs();
#endif /* !SBN_RECV_TASK */

    SBN_CheckSubscriptionPipe();

#ifndef SBN_SEND_TASK
    SBN_CheckPeerPipes();
#endif /* !SBN_SEND_TASK */

    if(Status == CFE_SB_NO_MESSAGE) Status = CFE_SUCCESS;

    CFE_ES_PerfLogExit(SBN_PERF_RECV_ID);

    return Status;
}/* end WaitForWakeup */

/**
 * Waits for either a response to the "get subscriptions" message from SB, OR
 * an event message that says SB has finished initializing. The latter message
 * means that SB was not started at the time SBN sent the "get subscriptions"
 * message, so that message will need to be sent again.
 * @return TRUE if message received was a initialization message and
 *      requests need to be sent again, or
 * @return FALSE if message received was a response
 */
static int WaitForSBStartup(void)
{
    CFE_EVS_Packet_t *EvsPacket = NULL;
    CFE_SB_MsgPtr_t SBMsgPtr = 0;
    uint8 counter = 0;
    CFE_SB_PipeId_t EventPipe = 0;
    uint32 Status = CFE_SUCCESS;

    DEBUG_START();

    /* Create event message pipe */
    Status = CFE_SB_CreatePipe(&EventPipe, 100, "SBNEventPipe");
    if(Status != CFE_SUCCESS)
    {
        CFE_EVS_SendEvent(SBN_INIT_EID, CFE_EVS_ERROR,
            "failed to create event pipe (%d)", (int)Status);
        return SBN_ERROR;
    }/* end if */

    /* Subscribe to event messages temporarily to be notified when SB is done
     * initializing
     */
    Status = CFE_SB_Subscribe(CFE_EVS_EVENT_MSG_MID, EventPipe);
    if(Status != CFE_SUCCESS)
    {
        CFE_EVS_SendEvent(SBN_INIT_EID, CFE_EVS_ERROR,
            "failed to subscribe to event pipe (%d)", (int)Status);
        return SBN_ERROR;
    }/* end if */

    while(1)
    {
        /* Check for subscription message from SB */
        if(SBN_CheckSubscriptionPipe())
        {
            /* SBN does not need to re-send request messages to SB */
            break;
        }
        else if(counter % 100 == 0)
        {
            /* Send subscription request messages again. This may cause the SB
             * to respond to duplicate requests but that should be okay
             */
            SBN_SendSubsRequests();
        }/* end if */

        /* Check for event message from SB */
        if(CFE_SB_RcvMsg(&SBMsgPtr, EventPipe, 100) == CFE_SUCCESS)
        {
            if(CFE_SB_GetMsgId(SBMsgPtr) == CFE_EVS_EVENT_MSG_MID)
            {
                EvsPacket = (CFE_EVS_Packet_t *)SBMsgPtr;

                /* If it's an event message from SB, make sure it's the init
                 * message
                 */
                if(strcmp(EvsPacket->
SBN_PAYLOAD
AppName, "CFE_SB") == 0
                    && EvsPacket->
SBN_PAYLOAD
EventID == CFE_SB_INIT_EID)
                {
                    break;
                }/* end if */
            }/* end if */
        }/* end if */

        counter++;
    }/* end while */

    /* Unsubscribe from event messages */
    CFE_SB_Unsubscribe(CFE_EVS_EVENT_MSG_MID, EventPipe);

    CFE_SB_DeletePipe(EventPipe);

    /* SBN needs to re-send request messages */
    return TRUE;
}/* end WaitForSBStartup */

/** \brief Initializes SBN */
static int Init(void)
{
    int Status = CFE_SUCCESS;
    uint32 TskId = 0;

    Status = CFE_ES_RegisterApp();
    if(Status != CFE_SUCCESS) return Status;

    Status = CFE_EVS_Register(NULL, 0, CFE_EVS_BINARY_FILTER);
    if(Status != CFE_SUCCESS) return Status;

    DEBUG_START();

    memset(&SBN, 0, sizeof(SBN));
    CFE_SB_InitMsg(&SBN.Hk, SBN_TLM_MID, sizeof(SBN.Hk), TRUE);

    /* load the App_FullName so I can ignore messages I send out to SB */
    TskId = OS_TaskGetId();
    CFE_SB_GetAppTskName(TskId, SBN.App_FullName);

    if(SBN_ReadModuleFile() == SBN_ERROR)
    {
        CFE_EVS_SendEvent(SBN_FILE_EID, CFE_EVS_ERROR,
            "module file not found or data invalid");
        return SBN_ERROR;
    }/* end if */

    if(SBN_GetPeerFileData() == SBN_ERROR)
    {
        CFE_EVS_SendEvent(SBN_FILE_EID, CFE_EVS_ERROR,
            "peer file not found or data invalid");
        return SBN_ERROR;
    }/* end if */

    SBN_InitInterfaces();

    CFE_ES_GetAppID(&SBN.AppID);

    /* Create pipe for subscribes and unsubscribes from SB */
    Status = CFE_SB_CreatePipe(&SBN.SubPipe, SBN_SUB_PIPE_DEPTH, "SBNSubPipe");
    if(Status != CFE_SUCCESS)
    {
        CFE_EVS_SendEvent(SBN_INIT_EID, CFE_EVS_ERROR,
            "failed to create subscription pipe (Status=%d)", (int)Status);
        return SBN_ERROR;
    }/* end if */

    Status = CFE_SB_SubscribeLocal(CFE_SB_ALLSUBS_TLM_MID, SBN.SubPipe,
        SBN_MAX_ALLSUBS_PKTS_ON_PIPE);
    if(Status != CFE_SUCCESS)
    {
        CFE_EVS_SendEvent(SBN_INIT_EID, CFE_EVS_ERROR,
            "failed to subscribe to allsubs (Status=%d)", (int)Status);
        return SBN_ERROR;
    }/* end if */

    Status = CFE_SB_SubscribeLocal(CFE_SB_ONESUB_TLM_MID, SBN.SubPipe,
        SBN_MAX_ONESUB_PKTS_ON_PIPE);
    if(Status != CFE_SUCCESS)
    {
        CFE_EVS_SendEvent(SBN_INIT_EID, CFE_EVS_ERROR,
            "failed to subscribe to sub (Status=%d)", (int)Status);
        return SBN_ERROR;
    }/* end if */

    /* Create pipe for HK requests and gnd commands */
    Status = CFE_SB_CreatePipe(&SBN.CmdPipe, 20, "SBNCmdPipe");
    if(Status != CFE_SUCCESS)
    {
        CFE_EVS_SendEvent(SBN_INIT_EID, CFE_EVS_ERROR,
            "failed to create command pipe (%d)", (int)Status);
        return SBN_ERROR;
    }/* end if */

    Status = CFE_SB_Subscribe(SBN_CMD_MID, SBN.CmdPipe);
    if(Status == CFE_SUCCESS)
    {
        CFE_EVS_SendEvent(SBN_INIT_EID, CFE_EVS_INFORMATION,
            "Subscribed to command MID 0x%04X", SBN_CMD_MID);
    }
    else
    {
        CFE_EVS_SendEvent(SBN_INIT_EID, CFE_EVS_ERROR,
            "failed to subscribe to command pipe (%d)", (int)Status);
        return SBN_ERROR;
    }/* end if */

    Status = SBN_LoadTables(&SBN.TableHandle);
    if (Status != CFE_SUCCESS)
    {
        CFE_EVS_SendEvent(SBN_INIT_EID, CFE_EVS_ERROR,
            "SBN failed to load SBN.RemapTable (%d)", Status);
        return SBN_ERROR;
    }/* end if */

    CFE_TBL_GetAddress((void **)&SBN.RemapTable, SBN.TableHandle);

    CFE_EVS_SendEvent(SBN_INIT_EID, CFE_EVS_INFORMATION,
        "initialized (CFE_CPU_NAME='%s' ProcessorID=%d SpacecraftId=%d %s "
        "SBN.AppID=%d...",
        CFE_CPU_NAME, CFE_PSP_GetProcessorId(), CFE_PSP_GetSpacecraftId(),
#ifdef SOFTWARE_BIG_BIT_ORDER
        "big-endian",
#else /* !SOFTWARE_BIG_BIT_ORDER */
        "little-endian",
#endif /* SOFTWARE_BIG_BIT_ORDER */
        (int)SBN.AppID);
    CFE_EVS_SendEvent(SBN_INIT_EID, CFE_EVS_INFORMATION,
        "...SBN_IDENT=%s SBN_DEBUG_MSGS=%s CMD_MID=0x%04X conf=%s)",
        SBN_IDENT,
#ifdef SBN_DEBUG_MSGS
        "TRUE",
#else /* !SBN_DEBUG_MSGS */
        "FALSE",
#endif /* SBN_DEBUG_MSGS */
        SBN_CMD_MID,
#ifdef CFE_ES_CONFLOADER
        "cfe_es_conf"
#else /* !CFE_ES_CONFLOADER */
        "scanf"
#endif /* CFE_ES_CONFLOADER */
        );

    SBN_InitializeCounters();

    /* Wait for event from SB saying it is initialized OR a response from SB
       to the above messages. TRUE means it needs to re-send subscription
       requests */
    if(WaitForSBStartup()) SBN_SendSubsRequests();

    return SBN_SUCCESS;
}/* end Init */

/** \brief SBN Main Routine */
void SBN_AppMain(void)
{
    int     Status = CFE_SUCCESS;
    uint32  RunStatus = CFE_ES_APP_RUN;

    Status = Init();
    if(Status != CFE_SUCCESS) RunStatus = CFE_ES_APP_ERROR;

    /* Loop Forever */
    while(CFE_ES_RunLoop(&RunStatus)) WaitForWakeup(SBN_MAIN_LOOP_DELAY);

    CFE_ES_ExitApp(RunStatus);
}/* end SBN_AppMain */

/**
 * Sends a message to a peer.
 * @param[in] MsgType The type of the message (application data, SBN protocol)
 * @param[in] CpuID The CpuID to send this message to.
 * @param[in] MsgSize The size of the message (in bytes).
 * @param[in] Msg The message contents.
 */
void SBN_ProcessNetMsg(SBN_NetInterface_t *Net, SBN_MsgType_t MsgType,
    SBN_CpuID_t CpuID, SBN_MsgSize_t MsgSize, void *Msg)
{
    int Status = 0;

    DEBUG_START();

    SBN_PeerInterface_t *Peer = SBN_GetPeer(Net, CpuID);

    if(!Peer)
    {
        return;
    }/* end if */

    if(MsgType == SBN_ANNOUNCE_MSG)
    {
        if(MsgSize < 1)
        {
            CFE_EVS_SendEvent(SBN_PEER_EID, CFE_EVS_INFORMATION,
                "peer running old (unversioned) code!");
        }
        else
        {
            if(strncmp(SBN_IDENT, (char *)Msg, SBN_IDENT_LEN))
            {
                CFE_EVS_SendEvent(SBN_PEER_EID, CFE_EVS_INFORMATION,
                    "%s version mismatch (me=%s, him=%s)",
                    Peer->Status.Name, SBN_IDENT, (char *)Msg);
            }
            else
            {
                CFE_EVS_SendEvent(SBN_PEER_EID, CFE_EVS_INFORMATION,
                    "%s running same version of SBN (%s)",
                    Peer->Status.Name, SBN_IDENT);
            }/* end if */
        }/* end if */
    }/* end if */

    if(Peer->Status.State == SBN_ANNOUNCING || MsgType == SBN_ANNOUNCE_MSG)
    {
        CFE_EVS_SendEvent(SBN_PEER_EID, CFE_EVS_INFORMATION,
            "peer %s alive", Peer->Status.Name);
        Peer->Status.State = SBN_HEARTBEATING;

        SBN_SendLocalSubsToPeer(Peer);
    }/* end if */

    switch(MsgType)
    {
        case SBN_ANNOUNCE_MSG:
        case SBN_HEARTBEAT_MSG:
            break;

        case SBN_APP_MSG:
            Status = CFE_SB_SendMsgFull(Msg,
                CFE_SB_DO_NOT_INCREMENT, CFE_SB_SEND_ONECOPY);

            if(Status != CFE_SUCCESS)
            {
                CFE_EVS_SendEvent(SBN_SB_EID, CFE_EVS_ERROR,
                    "CFE_SB_SendMsg error (Status=%d MsgType=0x%x)",
                    Status, MsgType);
            }/* end if */
            break;

        case SBN_SUBSCRIBE_MSG:
        {
            SBN_ProcessSubFromPeer(Peer, Msg);
            break;
        }

        case SBN_UN_SUBSCRIBE_MSG:
        {
            SBN_ProcessUnsubFromPeer(Peer, Msg);
            break;
        }

        default:
            /* make sure of termination */
            CFE_EVS_SendEvent(SBN_MSG_EID, CFE_EVS_ERROR,
                "unknown message type (MsgType=0x%x)", MsgType);
            break;
    }/* end switch */
}/* end SBN_ProcessNetMsg */

/**
 * Find the PeerIndex for a given CpuID and net.
 * @param[in] Net The network interface to search.
 * @param[in] ProcessorID The CpuID of the peer being sought.
 * @return The Peer interface pointer, or NULL if not found.
 */
SBN_PeerInterface_t *SBN_GetPeer(SBN_NetInterface_t *Net, uint32 ProcessorID)
{
    int PeerIdx = 0;

    /* DEBUG_START(); chatty */

    for(PeerIdx = 0; PeerIdx < Net->Status.PeerCount; PeerIdx++)
    {
        if(Net->Peers[PeerIdx].Status.ProcessorID == ProcessorID)
        {
            return &Net->Peers[PeerIdx];
        }/* end if */
    }/* end for */

    return NULL;
}/* end SBN_GetPeer */
