/*********************************************************
 * Copyright (C) 2003-2015 VMware, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation version 2.1 and no later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the Lesser GNU General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA.
 *
 *********************************************************/

/*
 * foundryToolsDaemon.c --
 *
 *    VIX-specific TCLO cmds that are called through the backdoor
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <fcntl.h>
#if defined(linux)
#include <sys/wait.h>
#include <mntent.h>
#include <paths.h>
#endif


#ifdef _WIN32
#include <io.h>
#else
#include <errno.h>
#include <unistd.h>
#endif

#ifdef _MSC_VER
#   include <Windows.h>
#   include <WinSock2.h>
#   include <WinSpool.h>
#   include "win32u.h"
#elif _WIN32
#   include "win95.h"
#endif

#include "vmware.h"
#include "procMgr.h"
#include "vm_version.h"
#include "message.h"

#include "vixPluginInt.h"
#include "vmware/tools/utils.h"

#include "util.h"
#include "strutil.h"
#include "str.h"
#include "file.h"
#include "err.h"
#include "hostinfo.h"
#include "guest_os.h"
#include "guest_msg_def.h"
#include "conf.h"
#include "vixCommands.h"
#include "base64.h"
#include "syncDriver.h"
#include "hgfsServerManager.h"
#include "hgfs.h"
#include "system.h"
#include "codeset.h"
#include "vixToolsInt.h"

#if defined(linux)
#include "hgfsDevLinux.h"
#endif

/* Only Win32, Linux, Solaris and FreeBSD use impersonation functions. */
#if !defined(__APPLE__)
#include "impersonate.h"
#endif

#include "vixOpenSource.h"

#define MAX64_DECIMAL_DIGITS 20          /* 2^64 = 18,446,744,073,709,551,616 */

#if defined(linux) || defined(_WIN32)

# if defined(_WIN32)
#  define DECLARE_SYNCDRIVER_ERROR(name) DWORD name = ERROR_SUCCESS
#  define SYNCDRIVERERROR ERROR_GEN_FAILURE
# else
#  define DECLARE_SYNCDRIVER_ERROR(name) int name = 0
#  define SYNCDRIVERERROR errno
# endif

static SyncDriverHandle gSyncDriverHandle = SYNCDRIVER_INVALID_HANDLE;

static Bool ToolsDaemonSyncDriverThawCallback(void *clientData);
#endif

static char *ToolsDaemonTcloGetQuotedString(const char *args,
                                            const char **endOfArg);
  
static VixError ToolsDaemonTcloGetEncodedQuotedString(const char *args,
                                                      const char **endOfArg,
                                                      char **result);

gboolean ToolsDaemonTcloReceiveVixCommand(RpcInData *data);

static HgfsServerMgrData gFoundryHgfsBkdrConn;
gboolean ToolsDaemonHgfsImpersonated(RpcInData *data);

#if defined(linux) || defined(_WIN32)
gboolean ToolsDaemonTcloSyncDriverFreeze(RpcInData *data);

gboolean ToolsDaemonTcloSyncDriverThaw(RpcInData *data);
#endif

gboolean ToolsDaemonTcloMountHGFS(RpcInData *data);

void ToolsDaemonTcloReportProgramCompleted(const char *requestName,
                                           VixError err,
                                           int exitCode,
                                           int64 pid,
                                           void *clientData);

/*
 * These constants are a bad hack. I really should generate the result 
 * strings twice, once to compute the length and then allocate the buffer, 
 * and a second time to write the buffer.
 */
#define DEFAULT_RESULT_MSG_MAX_LENGTH     1024

static Bool thisProcessRunsAsRoot = FALSE;


/*
 *-----------------------------------------------------------------------------
 *
 * FoundryToolsDaemonRunProgram --
 *
 *    Run a named program on the guest.
 *
 * Return value:
 *    TRUE on success
 *    FALSE on failure
 *
 * Side effects:
 *    None
 *
 *-----------------------------------------------------------------------------
 */

gboolean
FoundryToolsDaemonRunProgram(RpcInData *data) // IN
{
   VixError err = VIX_OK;
   char *requestName = NULL;
   char *commandLine = NULL;
   char *commandLineArgs = NULL;
   char *credentialTypeStr = NULL;
   char *obfuscatedNamePassword = NULL;
   char *directoryPath = NULL;
   char *environmentVariables = NULL;
   static char resultBuffer[DEFAULT_RESULT_MSG_MAX_LENGTH];
   Bool impersonatingVMWareUser = FALSE;
   void *userToken = NULL;
   ProcMgr_Pid pid;
   GMainLoop *eventQueue = ((ToolsAppCtx *)data->appCtx)->mainLoop;

   /*
    * Parse the arguments. Some of these are optional, so they
    * may be NULL.
    */
   requestName = ToolsDaemonTcloGetQuotedString(data->args, &data->args);

   err = ToolsDaemonTcloGetEncodedQuotedString(data->args, &data->args,
                                               &commandLine);
   if (err != VIX_OK) {
      goto abort;
   }

   err = ToolsDaemonTcloGetEncodedQuotedString(data->args, &data->args,
                                               &commandLineArgs);
   if (err != VIX_OK) {
      goto abort;
   }

   credentialTypeStr = ToolsDaemonTcloGetQuotedString(data->args, &data->args);
   obfuscatedNamePassword = ToolsDaemonTcloGetQuotedString(data->args, &data->args);
   directoryPath = ToolsDaemonTcloGetQuotedString(data->args, &data->args);
   environmentVariables = ToolsDaemonTcloGetQuotedString(data->args, &data->args);

   /*
    * Make sure we are passed the correct arguments.
    * Some of these arguments (like credentialTypeStr and obfuscatedNamePassword) are optional,
    * so they may be NULL.
    */
   if ((NULL == requestName) || (NULL == commandLine)) {
      err = VIX_E_INVALID_ARG;
      goto abort;
   }

   if ((NULL != credentialTypeStr)
         && (*credentialTypeStr) 
         && (thisProcessRunsAsRoot)) {
      impersonatingVMWareUser = VixToolsImpersonateUserImpl(credentialTypeStr, 
                                                            VIX_USER_CREDENTIAL_NONE,
                                                            obfuscatedNamePassword, 
                                                            &userToken);
      if (!impersonatingVMWareUser) {
         err = VIX_E_GUEST_USER_PERMISSIONS;
         goto abort;
      }
   }

   err = VixToolsRunProgramImpl(requestName,
                                commandLine,
                                commandLineArgs,
                                0,
                                userToken,
                                eventQueue,
                                (int64 *) &pid);

abort:
   if (impersonatingVMWareUser) {
      VixToolsUnimpersonateUser(userToken);
   }
   VixToolsLogoutUser(userToken);

   /*
    * All VMXI tools commands return results that start with a VMXI error
    * and a guest-OS-specific error.
    */
   Str_Sprintf(resultBuffer,
               sizeof(resultBuffer),
               "%"FMT64"d %d %"FMT64"d",
               err,
               Err_Errno(),
               (int64) pid);
   RPCIN_SETRETVALS(data, resultBuffer, TRUE);

   /*
    * These were allocated by ToolsDaemonTcloGetQuotedString.
    */
   free(requestName);
   free(commandLine);
   free(credentialTypeStr);
   free(obfuscatedNamePassword);
   free(directoryPath);
   free(environmentVariables);
   free(commandLineArgs);

   return TRUE;
} // FoundryToolsDaemonRunProgram


/*
 *-----------------------------------------------------------------------------
 *
 * FoundryToolsDaemonGetToolsProperties --
 *
 *    Get information about test features.
 *
 * Return value:
 *    TRUE on success
 *    FALSE on failure
 *
 * Side effects:
 *    None
 *
 *-----------------------------------------------------------------------------
 */

gboolean
FoundryToolsDaemonGetToolsProperties(RpcInData *data) // IN
{
   VixError err = VIX_OK;
   int additionalError = 0;
   static char resultBuffer[DEFAULT_RESULT_MSG_MAX_LENGTH];
   char *serializedBuffer = NULL;
   size_t serializedBufferLength = 0;
   char *base64Buffer = NULL;
   size_t base64BufferLength = 0;
   Bool success;
   char *returnBuffer = NULL;
   GKeyFile *confDictRef;
   
   /*
    * Collect some values about the host.
    */
   confDictRef = data->clientData;

   err = VixTools_GetToolsPropertiesImpl(confDictRef,
                                         &serializedBuffer,
                                         &serializedBufferLength);
   if (VIX_OK == err) {
      base64BufferLength = Base64_EncodedLength(serializedBuffer, serializedBufferLength) + 1;
      base64Buffer = Util_SafeMalloc(base64BufferLength);
      success = Base64_Encode(serializedBuffer, 
                              serializedBufferLength, 
                              base64Buffer, 
                              base64BufferLength, 
                              &base64BufferLength);
      if (!success) {
         base64Buffer[0] = 0;
         err = VIX_E_FAIL;
         goto abort;
      }
      base64Buffer[base64BufferLength] = 0;
   }


abort:
   returnBuffer = base64Buffer;
   if (NULL == base64Buffer) {
      returnBuffer = "";
   }
   if (VIX_OK != err) {
      additionalError = Err_Errno();
   }

   /*
    * All VMXI tools commands return results that start with a VMXI error
    * and a guest-OS-specific error.
    */
   Str_Sprintf(resultBuffer,
               sizeof(resultBuffer),
               "%"FMT64"d %d %s",
               err,
               additionalError,
               returnBuffer);
   RPCIN_SETRETVALS(data, resultBuffer, TRUE);

   free(serializedBuffer);
   free(base64Buffer);
   
   return TRUE;
} // FoundryToolsDaemonGetToolsProperties


/**
 * Initializes internal state of the Foundry daemon.
 *
 * @param[in]  ctx      Application context.
 */

void
FoundryToolsDaemon_Initialize(ToolsAppCtx *ctx)
{
   thisProcessRunsAsRoot = (strcmp(ctx->name, VMTOOLS_GUEST_SERVICE) == 0);

   /*
    * TODO: Add the original/native environment (envp) to ToolsAppContext so
    * we can know what the environment variables were before the loader scripts
    * changed them.
    */
   (void) VixTools_Initialize(thisProcessRunsAsRoot,
#if defined(__FreeBSD__)
                              ctx->envp,   // envp
#else
                              NULL,        // envp
#endif
                              ToolsDaemonTcloReportProgramCompleted,
                              ctx);

#if !defined(__APPLE__)
   if (thisProcessRunsAsRoot) {
      Impersonate_Init();
   }
#endif

   /* Register a straight through connection with the Hgfs server. */
   HgfsServerManager_DataInit(&gFoundryHgfsBkdrConn,
                              VIX_BACKDOORCOMMAND_SEND_HGFS_PACKET,
                              NULL,    // rpc - no rpc registered
                              NULL);   // rpc callback
   HgfsServerManager_Register(&gFoundryHgfsBkdrConn);

}


/**
 * Uninitializes internal state of the Foundry daemon.
 *
 * @param[in]  ctx      Application context.
 */

void
FoundryToolsDaemon_Uninitialize(ToolsAppCtx *ctx)
{
   HgfsServerManager_Unregister(&gFoundryHgfsBkdrConn);
   VixTools_Uninitialize();
}


/**
 * Restrict VIX commands in Foundry daemon.
 *
 * @param[in]  ctx        Application context.
 * @param[in]  restricted TRUE/FALSE=>enable/disable restriction.
 */

void
FoundryToolsDaemon_RestrictVixCommands(ToolsAppCtx *ctx, gboolean restricted)
{
   VixTools_RestrictCommands(restricted);
}


/*
 *-----------------------------------------------------------------------------
 *
 * ToolsDaemonTcloGetQuotedString --
 *
 *    Extract a quoted string from the middle of an argument string.
 *    This is different than normal tokenizing in a few ways:
 *       * Whitespace is a separator outside quotes, but not inside quotes.
 *       * Quotes always come in pairs, so "" is am empty string. An empty
 *          string may appear anywhere in the string, even at the end, so
 *          a string that is "" contains 1 empty string, not 2.
 *       * The string may use whitespace to separate the op-name from the params,
 *          and then quoted params to skip whitespace inside a param.
 *
 * Return value:
 *    Allocates the string.
 *
 * Side effects:
 *    None
 *
 *-----------------------------------------------------------------------------
 */

static char *
ToolsDaemonTcloGetQuotedString(const char *args,      // IN
                               const char **endOfArg) // OUT
{
   char *resultStr = NULL;
   char *endStr;
   Debug(">ToolsDaemonTcloGetQuotedString\n");

   while ((*args) && ('\"' != *args)) {
      args++;
   }
   if ('\"' == *args) {
      args++;
   }

   resultStr = Util_SafeStrdup(args);

   endStr = resultStr;
   while (*endStr) {
      if (('\\' == *endStr) && (*(endStr + 1))) {
         endStr += 2;
      } else if ('\"' == *endStr) {
         *endStr = 0;
         endStr++;
         break;
      } else {
         endStr++;
      }
   }

   if (NULL != endOfArg) {
      args += (endStr - resultStr);
      while (' ' == *args) {
         args++;
      }
      *endOfArg = args;
   }

   Debug("<ToolsDaemonTcloGetQuotedString\n");
   return resultStr;
} // ToolsDaemonTcloGetQuotedString


/*
 *-----------------------------------------------------------------------------
 *
 * ToolsDaemonTcloGetEncodedQuotedString --
 *
 *    This is a wrapper for ToolsDaemonTcloGetQuotedString.
 *    It just decoded the string.
 *
 * Return value:
 *    Allocates the string.
 *
 * Side effects:
 *    None
 *
 *-----------------------------------------------------------------------------
 */

static VixError
ToolsDaemonTcloGetEncodedQuotedString(const char *args,      // IN
                                      const char **endOfArg, // OUT
                                      char **result)         // OUT
{
   VixError err = VIX_OK;
   char *rawResultStr = NULL;
   char *resultStr = NULL;

   rawResultStr = ToolsDaemonTcloGetQuotedString(args, endOfArg);
   if (NULL == rawResultStr) {
      goto abort;
   }

   err = VixMsg_DecodeString(rawResultStr, &resultStr);

abort:
   free(rawResultStr);
   *result = resultStr;

   return err;
}

#if defined(linux) || defined(_WIN32)

/*
 *-----------------------------------------------------------------------------
 *
 * ToolsDaemonTcloSyncDriverFreeze --
 *
 *    Use the Sync Driver to freeze I/O in the guest..
 *
 * Return value:
 *    TRUE on success
 *    FALSE on failure
 *
 * Side effects:
 *    None
 *
 *-----------------------------------------------------------------------------
 */

gboolean
ToolsDaemonTcloSyncDriverFreeze(RpcInData *data)
{
   static char resultBuffer[DEFAULT_RESULT_MSG_MAX_LENGTH];
   VixError err = VIX_OK;
   char *driveList = NULL;
   char *timeout = NULL;
   int timeoutVal;
   DECLARE_SYNCDRIVER_ERROR(sysError);
   ToolsAppCtx *ctx = data->appCtx;
   GKeyFile *confDictRef = ctx->config;
   Bool enableNullDriver;
   GSource *timer;
   
   Debug(">ToolsDaemonTcloSyncDriverFreeze\n");

   /*
    * Parse the arguments
    */
   driveList = ToolsDaemonTcloGetQuotedString(data->args, &data->args);
   timeout = ToolsDaemonTcloGetQuotedString(data->args, &data->args);

   /*
    * Validate the arguments.
    */
   if (NULL == driveList || NULL == timeout) {
      err = VIX_E_INVALID_ARG;
      Debug("ToolsDaemonTcloSyncDriverFreeze: Failed to get string args\n");
      goto abort;
   }

   if (!StrUtil_StrToInt(&timeoutVal, timeout) || timeoutVal < 0) {
      Debug("ToolsDaemonTcloSyncDriverFreeze: Bad args, timeout '%s'\n", timeout);
      err = VIX_E_INVALID_ARG;
      goto abort;
   }

   Debug("SYNCDRIVE: Got request to freeze '%s', timeout %d\n", driveList,
         timeoutVal);

   /* Disallow multiple freeze calls. */
   if (gSyncDriverHandle != SYNCDRIVER_INVALID_HANDLE) {
      err = VIX_E_OBJECT_IS_BUSY;
      goto abort;
   }

   enableNullDriver = VixTools_ConfigGetBoolean(confDictRef,
                                                "vmbackup",
                                                "enableNullDriver",
                                                FALSE);

   /* Perform the actual freeze. */
   if (!SyncDriver_Freeze(driveList, enableNullDriver, &gSyncDriverHandle) ||
       SyncDriver_QueryStatus(gSyncDriverHandle, INFINITE) != SYNCDRIVER_IDLE) {
      Debug("ToolsDaemonTcloSyncDriverFreeze: Failed to Freeze drives '%s'\n",
            driveList);
      err = VIX_E_FAIL;
      sysError = SYNCDRIVERERROR;
      if (gSyncDriverHandle != SYNCDRIVER_INVALID_HANDLE) {
         SyncDriver_Thaw(gSyncDriverHandle);
         SyncDriver_CloseHandle(&gSyncDriverHandle);
      }
      goto abort;
   }

   /* Start the timer callback to automatically thaw. */
   if (0 != timeoutVal) {
      Debug("ToolsDaemonTcloSyncDriverFreeze: Starting timer callback %d\n", timeoutVal);
      timer = g_timeout_source_new(timeoutVal * 10);
      VMTOOLSAPP_ATTACH_SOURCE(ctx, timer, ToolsDaemonSyncDriverThawCallback, NULL, NULL);
      g_source_unref(timer);
   }

abort:
   /*
    * These were allocated by ToolsDaemonTcloGetQuotedString.
    */
   free(driveList);
   free(timeout);

   /*
    * All Foundry tools commands return results that start with a
    * foundry error and a guest-OS-specific error.
    */
   Str_Sprintf(resultBuffer, sizeof resultBuffer, "%"FMT64"d %d", err, sysError);
   Debug("<ToolsDaemonTcloSyncDriverFreeze\n");
   return RPCIN_SETRETVALS(data, resultBuffer, TRUE);
}
#endif


/*
 *-----------------------------------------------------------------------------
 *
 * ToolsDaemonSyncDriverThawCallback --
 *
 *      Callback to thaw all currently frozen drives if they have not been
 *      thawed already.
 *
 * Results:
 *      TRUE (returning FALSE will stop the event loop)
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

#if defined(linux) || defined(_WIN32)
static Bool
ToolsDaemonSyncDriverThawCallback(void *clientData) // IN (ignored)
{
   Debug(">ToolsDaemonSyncDriverThawCallback\n");
   Debug("ToolsDaemonSyncDriverThawCallback: Timed out waiting for thaw.\n");

   if (gSyncDriverHandle == SYNCDRIVER_INVALID_HANDLE) {
      Debug("<ToolsDaemonSyncDriverThawCallback\n");
      Debug("ToolsDaemonSyncDriverThawCallback: No drives are frozen.\n");
      goto exit;
   }
   if (!SyncDriver_Thaw(gSyncDriverHandle)) {
      Debug("ToolsDaemonSyncDriverThawCallback: Failed to thaw.\n");
   }

exit:
   SyncDriver_CloseHandle(&gSyncDriverHandle);
   Debug("<ToolsDaemonSyncDriverThawCallback\n");
   return TRUE;
}
#endif


/*
 *-----------------------------------------------------------------------------
 *
 * ToolsDaemonTcloSyncDriverThaw --
 *
 *    Thaw I/O previously frozen by the Sync Driver.
 *
 * Return value:
 *    TRUE on success
 *    FALSE on failure
 *
 * Side effects:
 *    None
 *
 *-----------------------------------------------------------------------------
 */

#if defined(linux) || defined(_WIN32)
gboolean
ToolsDaemonTcloSyncDriverThaw(RpcInData *data) // IN
{
   static char resultBuffer[DEFAULT_RESULT_MSG_MAX_LENGTH];
   VixError err = VIX_OK;
   DECLARE_SYNCDRIVER_ERROR(sysError);
   Debug(">ToolsDaemonTcloSyncDriverThaw\n");

   /*
    * This function has no arguments that we care about.
    */

   Debug("SYNCDRIVE: Got request to thaw\n");

   if (gSyncDriverHandle == SYNCDRIVER_INVALID_HANDLE) {
      err = VIX_E_GUEST_VOLUMES_NOT_FROZEN;
      sysError = SYNCDRIVERERROR;
      Debug("ToolsDaemonTcloSyncDriverThaw: No drives are frozen.\n");
   } else if (!SyncDriver_Thaw(gSyncDriverHandle)) {
      err = VIX_E_FAIL;
      sysError = SYNCDRIVERERROR;
      Debug("ToolsDaemonTcloSyncDriverThaw: Failed to Thaw drives\n");
   }

   SyncDriver_CloseHandle(&gSyncDriverHandle);

   /*
    * All Foundry tools commands return results that start with a
    * foundry error and a guest-OS-specific error.
    */
   Str_Sprintf(resultBuffer, sizeof resultBuffer, "%"FMT64"d %d", err, sysError);
   Debug("<ToolsDaemonTcloSyncDriverThaw\n");
   return RPCIN_SETRETVALS(data, resultBuffer, TRUE);
}
#endif


/*
 *-----------------------------------------------------------------------------
 *
 * ToolsDaemonTcloMountHGFS --
 *
 *
 * Return value:
 *    VixError
 *
 * Side effects:
 *    None
 *
 *-----------------------------------------------------------------------------
 */

gboolean
ToolsDaemonTcloMountHGFS(RpcInData *data) // IN
{
   VixError err = VIX_OK;
   static char resultBuffer[DEFAULT_RESULT_MSG_MAX_LENGTH];

#if defined(linux)
   /*
    * Look for a vmhgfs mount at /mnt/hgfs. If one exists, nothing
    * else needs to be done.  If one doesn't exist, then mount at
    * that location.
    */
   FILE *mtab;
   struct mntent *mnt;
   if ((mtab = setmntent(_PATH_MOUNTED, "r")) == NULL) {
      err = VIX_E_FAIL;
   } else {
      Bool vmhgfsMntFound = FALSE;
      while ((mnt = getmntent(mtab)) != NULL) {
         if ((strcmp(mnt->mnt_fsname, ".host:/") == 0) &&
             (strcmp(mnt->mnt_type, HGFS_NAME) == 0) &&
             (strcmp(mnt->mnt_dir, "/mnt/hgfs") == 0)) {
             vmhgfsMntFound = TRUE;
             break;
         }
      }
      endmntent(mtab);

      if (!vmhgfsMntFound) {
         /*
          * We need to call the mount program, not the mount system call. The
          * mount program does several additional things, like compute the mount
          * options from the contents of /etc/fstab, and invoke custom mount
          * programs like the one needed for HGFS.
          */
         int ret = system("mount -t vmhgfs .host:/ /mnt/hgfs");
         if (ret == -1 || WIFSIGNALED(ret) ||
             (WIFEXITED(ret) && WEXITSTATUS(ret) != 0)) {
            err = VIX_E_HGFS_MOUNT_FAIL;
         }
      }
   }
#endif

   /*
    * All tools commands return results that start with an error
    * and a guest-OS-specific error.
    */
   Str_Sprintf(resultBuffer,
               sizeof(resultBuffer),
               "%"FMT64"d %d",
               err,
               Err_Errno());
   RPCIN_SETRETVALS(data, resultBuffer, TRUE);

   return TRUE;
} // ToolsDaemonTcloMountHGFS


#if !defined(N_PLAT_NLM)
/*
 *-----------------------------------------------------------------------------
 *
 * ToolsDaemonHgfsImpersonated --
 *
 *      Tclo cmd handler for hgfs requests.
 *
 *      Here we receive guest user credentials and an HGFS packet to
 *      be processed by the HGFS server under the context of
 *      the guest user credentials.
 *
 *      We pre-allocate a HGFS reply packet buffer and leave some space at
 *      the beginning of the buffer for foundry error codes.
 *      The format of the foundry error codes is a 64 bit number (as text),
 *      followed by a 32 bit number (as text), followed by a hash,
 *      all delimited by space (' ').  The hash is needed
 *      to make it easier for text parsers to know where the
 *      HGFS reply packet begins, since it can start with a space.
 *
 *      We do this funky "allocate an HGFS packet with extra
 *      room for foundry error codes" to avoid copying buffers
 *      around.  The HGFS packet buffer is roughly 62k for large V3 Hgfs request
 *      or 6k for other request , so it would be bad to copy that for every packet.
 *
 *      It is guaranteed that we will not be called twice
 *      at the same time, so it is safe for resultPacket to be static.
 *      The TCLO processing loop (RpcInLoop()) is synchronous.
 *
 *
 * Results:
 *      TRUE on TCLO success (*result contains the hgfs reply)
 *      FALSE on TCLO error (not supposed to happen)
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

gboolean
ToolsDaemonHgfsImpersonated(RpcInData *data) // IN
{
   VixError err;
   size_t hgfsPacketSize = 0;
   size_t hgfsReplySize = 0;
   const char *origArgs = data->args;
   Bool impersonatingVMWareUser = FALSE;
   char *credentialTypeStr = NULL;
   char *obfuscatedNamePassword = NULL;
   void *userToken = NULL;
   int actualUsed;
#define STRLEN_OF_MAX_64_BIT_NUMBER_AS_STRING 20
#define OTHER_TEXT_SIZE 4                /* strlen(space zero space quote) */
   static char resultPacket[STRLEN_OF_MAX_64_BIT_NUMBER_AS_STRING
                              + OTHER_TEXT_SIZE
                              + HGFS_LARGE_PACKET_MAX];
   char *hgfsReplyPacket = resultPacket
                             + STRLEN_OF_MAX_64_BIT_NUMBER_AS_STRING
                             + OTHER_TEXT_SIZE;

   Debug(">ToolsDaemonHgfsImpersonated\n");

   err = VIX_OK;

   /*
    * We assume VixError is 64 bits.  If it changes, we need
    * to adjust STRLEN_OF_MAX_64_BIT_NUMBER_AS_STRING.
    *
    * There isn't much point trying to return gracefully
    * if sizeof(VixError) is larger than we expected: we didn't
    * allocate enough space to actually represent the error!
    * So we're stuck.  Panic at this point.
    */
   ASSERT_ON_COMPILE(sizeof (uint64) == sizeof err);

   /*
    * Get the authentication information.
    */
   credentialTypeStr = ToolsDaemonTcloGetQuotedString(data->args, &data->args);
   obfuscatedNamePassword = ToolsDaemonTcloGetQuotedString(data->args, &data->args);

   /*
    * Make sure we are passed the correct arguments.
    */
   if ((NULL == credentialTypeStr) || (NULL == obfuscatedNamePassword)) {
      err = VIX_E_INVALID_ARG;
      goto abort;
   }

   /*
    * Skip over our token that is right before the HGFS packet.
    * This makes ToolsDaemonTcloGetQuotedString parsing predictable,
    * since it will try to eat trailing spaces after a quoted string,
    * and the HGFS packet might begin with a space.
    */
   if (((data->args - origArgs) >= data->argsSize) || ('#' != *(data->args))) {
      /*
       * Buffer too small or we got an unexpected token.
       */
      err = VIX_E_FAIL;
      goto abort;
   }
   data->args++;
   
   /*
    * At this point args points to the HGFS packet.
    * If we're pointing beyond the end of the buffer, we'll
    * get a negative HGFS packet length and abort.
    */
   hgfsPacketSize = data->argsSize - (data->args - origArgs);
   if (hgfsPacketSize <= 0) {
      err = VIX_E_FAIL;
      goto abort;
   }
   
   if (thisProcessRunsAsRoot) {
      impersonatingVMWareUser = VixToolsImpersonateUserImpl(credentialTypeStr,
                                                            VIX_USER_CREDENTIAL_NONE,
                                                            obfuscatedNamePassword,
                                                            &userToken);
      if (!impersonatingVMWareUser) {
         err = VIX_E_GUEST_USER_PERMISSIONS;
         goto abort;
      }
   }

   /*
    * Impersonation was okay, so let's give our packet to
    * the HGFS server and forward the reply packet back.
    */
   hgfsReplySize = sizeof resultPacket - (hgfsReplyPacket - resultPacket);
   HgfsServerManager_ProcessPacket(&gFoundryHgfsBkdrConn, // hgfs server connection
                                   data->args,            // packet in buf
                                   hgfsPacketSize,        // packet in size
                                   hgfsReplyPacket,       // packet out buf
                                   &hgfsReplySize);       // reply buf/data size

abort:
   if (impersonatingVMWareUser) {
      VixToolsUnimpersonateUser(userToken);
   }
   VixToolsLogoutUser(userToken);

   /*
    * These were allocated by ToolsDaemonTcloGetQuotedString.
    */
   free(credentialTypeStr);
   free(obfuscatedNamePassword);

   data->result = resultPacket;
   data->resultLen = STRLEN_OF_MAX_64_BIT_NUMBER_AS_STRING
                        + OTHER_TEXT_SIZE
                        + hgfsReplySize;
   
   /*
    * Render the foundry error codes into the buffer.
    */
   actualUsed = Str_Snprintf(resultPacket,
                             STRLEN_OF_MAX_64_BIT_NUMBER_AS_STRING
                               + OTHER_TEXT_SIZE,
                             "%"FMT64"d 0 ",
                             err);
                             
   if (actualUsed < 0) {
      /*
       * We computed our string length wrong!  This should never happen.
       * But if it does, let's try to recover gracefully.  The "1" in
       * the string below is VIX_E_FAIL.  We don't try to use %d since
       * we couldn't even do that right the first time around.
       * That hash is needed for the parser on the other
       * end to stop before the HGFS packet, since the HGFS packet
       * can contain a space (and the parser can eat trailing spaces).
       */
      ASSERT(0);
      actualUsed = Str_Snprintf(resultPacket,
                                STRLEN_OF_MAX_64_BIT_NUMBER_AS_STRING,
                                "1 0 #");
      data->resultLen = actualUsed;
   } else {
      /*
       * We computed the string length correctly.  Great!
       *
       * We allocated enough space to cover a large 64 bit number
       * for VixError.  Chances are we didn't use all that space.
       * Instead, pad it with whitespace so the text parser can skip
       * over it.
       */
      memset(resultPacket + actualUsed,
             ' ',
             STRLEN_OF_MAX_64_BIT_NUMBER_AS_STRING
                                 + OTHER_TEXT_SIZE
                                 - actualUsed);   
      /*
       * Put a hash right before the HGFS packet.
       * So the buffer will look something like this:
       * "0 0                        #" followed by the HGFS packet.
       */
      resultPacket[STRLEN_OF_MAX_64_BIT_NUMBER_AS_STRING
                    + OTHER_TEXT_SIZE - 1] = '#';
   }

   Debug("<<<ToolsDaemonHgfsImpersonated\n");
   return TRUE;
} // ToolsDaemonHgfsImpersonated

#undef STRLEN_OF_MAX_64_BIT_NUMBER_AS_STRING
#undef OTHER_TEXT_SIZE
#endif


/*
 *-----------------------------------------------------------------------------
 *
 * ToolsDaemonTcloReportProgramCompleted --
 *
 *
 * Return value:
 *
 * Side effects:
 *    None
 *
 *-----------------------------------------------------------------------------
 */

void
ToolsDaemonTcloReportProgramCompleted(const char *requestName,    // IN
                                      VixError err,               // IN
                                      int exitCode,               // IN
                                      int64 pid,                  // IN
                                      void *clientData)           // IN
{
   Bool sentResult;
   ToolsAppCtx *ctx = clientData;
   gchar *msg = g_strdup_printf("%s %s %"FMT64"d %d %d %"FMT64"d",
                                VIX_BACKDOORCOMMAND_RUN_PROGRAM_DONE,
                                requestName,
                                err,
                                Err_Errno(),
                                exitCode,
                                (int64) pid);

   sentResult = RpcChannel_Send(ctx->rpc, msg, strlen(msg) + 1, NULL, NULL);
   g_free(msg);

   if (!sentResult) {
      Warning("Unable to send results from polling the result program.\n\n");
   }
} // ToolsDaemonTcloReportProgramCompleted


/*
 *-----------------------------------------------------------------------------
 *
 * ToolsDaemonTcloReceiveVixCommand --
 *
 *
 * Return value:
 *    TRUE on success
 *    FALSE on failure
 *
 * Side effects:
 *    None
 *
 *-----------------------------------------------------------------------------
 */

gboolean
ToolsDaemonTcloReceiveVixCommand(RpcInData *data) // IN
{
   VixError err = VIX_OK;
   uint32 additionalError = 0;
   char *requestName = NULL;
   VixCommandRequestHeader *requestMsg = NULL;
   size_t maxResultBufferSize;
   size_t tcloBufferLen;
   char *resultValue = NULL;
   size_t resultValueLength = 0;
   Bool deleteResultValue = FALSE;
   char *destPtr = NULL;
   int vixPrefixDataSize = (MAX64_DECIMAL_DIGITS * 2)
                             + (sizeof(' ') * 2)
                             + sizeof('\0')
                             + sizeof(' ') * 10;   // for RPC header

   /*
    * Our temporary buffer will be the same size as what the
    * Tclo/RPC system can handle, which is GUESTMSG_MAX_IN_SIZE.
    */
   static char tcloBuffer[GUESTMSG_MAX_IN_SIZE];

   ToolsAppCtx *ctx = data->appCtx;
   GMainLoop *eventQueue = ctx->mainLoop;
   GKeyFile *confDictRef = ctx->config;

   requestName = ToolsDaemonTcloGetQuotedString(data->args, &data->args);

   /*
    * Skip the NULL, char, and then the rest of the buffer should just 
    * be a Vix command object.
    */
   while (*data->args) {
      data->args += 1;
   }
   data->args += 1;
   err = VixMsg_ValidateMessage((char *) data->args, data->argsSize);
   if (VIX_OK != err) {
      goto abort;
   }
   requestMsg = (VixCommandRequestHeader *) data->args;
   maxResultBufferSize = sizeof(tcloBuffer) - vixPrefixDataSize;

   err = VixTools_ProcessVixCommand(requestMsg,
                                    requestName,
                                    maxResultBufferSize,
                                    confDictRef,
                                    eventQueue,
                                    &resultValue,
                                    &resultValueLength,
                                    &deleteResultValue);

   /*
    * NOTE: We have always been returning an additional 32 bit error (errno,
    * or GetLastError() for Windows) along with the 64 bit VixError. The VMX
    * side has been dropping the higher order 32 bits of VixError (by copying
    * it onto a 32 bit error). They do save the additional error but as far
    * as we can tell, it was not getting used by foundry. So at this place,
    * for certain guest commands that have extra error information tucked into
    * the higher order 32 bits of the VixError, we use that extra error as the
    * additional error to be sent back to VMX.
    */
   additionalError = VixTools_GetAdditionalError(requestMsg->opCode, err);
   Debug("%s: additionalError = %u\n", __FUNCTION__, additionalError);

abort:
   tcloBufferLen = resultValueLength + vixPrefixDataSize;

   /*
    * If we generated a message larger than tclo/Rpc can handle,
    * we did something wrong.  Our code should never have done this.
    */
   if (tcloBufferLen > sizeof tcloBuffer) {
      ASSERT(0);
      resultValue[0] = 0;
      tcloBufferLen = tcloBufferLen - resultValueLength;
      err = VIX_E_OUT_OF_MEMORY;
   }

   /*
    * All Foundry tools commands return results that start with a foundry error
    * and a guest-OS-specific error.
    */
   Str_Sprintf(tcloBuffer,
               sizeof tcloBuffer,
               "%"FMT64"d %d ",
               err,
               additionalError);
   destPtr = tcloBuffer + strlen(tcloBuffer);

   /*
    * If this is a binary result, then we put a # at the end of the ascii to
    * mark the end of ascii and the start of the binary data. 
    */
   if ((NULL != requestMsg)
         && (requestMsg->commonHeader.commonFlags & VIX_COMMAND_GUEST_RETURNS_BINARY)) {
      *(destPtr++) = '#';
      data->resultLen = destPtr - tcloBuffer + resultValueLength;
   }

   /*
    * Copy the result. Don't use a strcpy, since this may be a binary buffer.
    */
   memcpy(destPtr, resultValue, resultValueLength);
   destPtr += resultValueLength;

   /*
    * If this is not binary data, then it should be a NULL terminated string.
    */
   if ((NULL == requestMsg)
         || !(requestMsg->commonHeader.commonFlags & VIX_COMMAND_GUEST_RETURNS_BINARY)) {
      *(destPtr++) = 0;
      data->resultLen = strlen(tcloBuffer) + 1;
   }
   
   data->result = tcloBuffer;

   if (deleteResultValue) {
      free(resultValue);
   }
   free(requestName);

   Debug("<ToolsDaemonTcloReceiveVixCommand\n");
   return TRUE;
} // ToolsDaemonTcloReceiveVixCommand

