/* ====================================================================
 * The Apache Software License, Version 1.1
 *
 * Copyright (c) 2000-2002 The Apache Software Foundation.  All rights
 * reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The end-user documentation included with the redistribution,
 *    if any, must include the following acknowledgment:
 *       "This product includes software developed by the
 *        Apache Software Foundation (http://www.apache.org/)."
 *    Alternately, this acknowledgment may appear in the software itself,
 *    if and wherever such third-party acknowledgments normally appear.
 *
 * 4. The names "Apache" and "Apache Software Foundation" must
 *    not be used to endorse or promote products derived from this
 *    software without prior written permission. For written
 *    permission, please contact apache@apache.org.
 *
 * 5. Products derived from this software may not be called "Apache",
 *    nor may "Apache" appear in their name, without prior written
 *    permission of the Apache Software Foundation.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESSED OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE APACHE SOFTWARE FOUNDATION OR
 * ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 * ====================================================================
 *
 * This software consists of voluntary contributions made by many
 * individuals on behalf of the Apache Software Foundation.  For more
 * information on the Apache Software Foundation, please see
 * <http://www.apache.org/>.
 */

#include "win32/threadproc.h"
#include "win32/fileio.h"

#include "apr_thread_proc.h"
#include "apr_file_io.h"
#include "apr_general.h"
#include "apr_strings.h"
#include "apr_portable.h"
#include "apr_lib.h"
#include <stdlib.h>
#if APR_HAVE_SIGNAL_H
#include <signal.h>
#endif
#include <string.h>
#if APR_HAVE_PROCESS_H
#include <process.h>
#endif

#ifdef _WIN32_WCE
#ifndef DETACHED_PROCESS
#define DETACHED_PROCESS 0
#endif
#ifndef CREATE_UNICODE_ENVIRONMENT
#define CREATE_UNICODE_ENVIRONMENT 0
#endif
#ifndef STARTF_USESHOWWINDOW
#define STARTF_USESHOWWINDOW 0
#endif
#ifndef SW_HIDE
#define SW_HIDE 0
#endif
#endif
/* 
 * some of the ideas expressed herein are based off of Microsoft
 * Knowledge Base article: Q190351
 *
 */

APR_DECLARE(apr_status_t) apr_procattr_create(apr_procattr_t **new,
                                                  apr_pool_t *pool)
{
    (*new) = (apr_procattr_t *)apr_pcalloc(pool, sizeof(apr_procattr_t));
    (*new)->pool = pool;
    (*new)->cmdtype = APR_PROGRAM;
    return APR_SUCCESS;
}

static apr_status_t open_nt_process_pipe(apr_file_t **read, apr_file_t **write,
                                         apr_int32_t iBlockingMode,
                                         apr_pool_t *pool)
{
    apr_status_t stat;
    BOOLEAN bAsyncRead, bAsyncWrite;

    switch (iBlockingMode) {
    case APR_FULL_BLOCK:
        bAsyncRead = bAsyncWrite = FALSE;
        break;
    case APR_PARENT_BLOCK:
        bAsyncRead = FALSE;
        bAsyncWrite = TRUE;
        break;
    case APR_CHILD_BLOCK:
        bAsyncRead = TRUE;
        bAsyncWrite = FALSE;
        break;
    default:
        bAsyncRead = TRUE;
        bAsyncWrite = TRUE;
    }
    if ((stat = apr_create_nt_pipe(read, write, bAsyncRead, bAsyncWrite,
                                   pool)) != APR_SUCCESS)
        return stat;

    return APR_SUCCESS;
}

static apr_status_t make_handle_private(apr_file_t *file)
{
#ifdef _WIN32_WCE
    return APR_ENOTIMPL;
#else
    HANDLE hproc = GetCurrentProcess();
    HANDLE filehand = file->filehand;

    /* Create new non-inheritable versions of handles that
     * the child process doesn't care about. Otherwise, the child
     * inherits these handles; resulting in non-closeable handles
     * to the respective pipes.
     */
    if (!DuplicateHandle(hproc, filehand,
                         hproc, &file->filehand, 0,
                         FALSE, DUPLICATE_SAME_ACCESS))
        return apr_get_os_error();
    /* 
     * Close the inerhitable handle we don't need anymore.
     */
    CloseHandle(filehand);
    return APR_SUCCESS;
#endif
}

APR_DECLARE(apr_status_t) apr_procattr_io_set(apr_procattr_t *attr,
                                             apr_int32_t in, 
                                             apr_int32_t out,
                                             apr_int32_t err)
{
    apr_status_t stat = APR_SUCCESS;

    if (in) {
        stat = open_nt_process_pipe(&attr->child_in, &attr->parent_in, in,
                                    attr->pool);
        if (stat == APR_SUCCESS)
            stat = make_handle_private(attr->parent_in);
    }
    if (out && stat == APR_SUCCESS) {
        stat = open_nt_process_pipe(&attr->parent_out, &attr->child_out, out,
                                    attr->pool);
        if (stat == APR_SUCCESS)
            stat = make_handle_private(attr->parent_out);
    }
    if (err && stat == APR_SUCCESS) {
        stat = open_nt_process_pipe(&attr->parent_err, &attr->child_err, err,
                                    attr->pool);
        if (stat == APR_SUCCESS)
            stat = make_handle_private(attr->parent_err);
    }
    return stat;
}

APR_DECLARE(apr_status_t) apr_procattr_child_in_set(apr_procattr_t *attr, 
                                                  apr_file_t *child_in, 
                                                  apr_file_t *parent_in)
{
    apr_status_t rv = APR_SUCCESS;

    if (child_in) {
        if (attr->child_in == NULL)
            rv = apr_file_dup(&attr->child_in, child_in, attr_pool);
        else
            rv = apr_file_dup2(attr->child_in, child_in, attr_pool);

        if (rv == APR_SUCCESS)
            rv = apr_file_inherit_set(attr->child_in);
    }

    if (parent_in && rv == APR_SUCCESS) {
        if (attr->parent_in == NULL)
            rv = apr_file_dup(&attr->parent_in, parent_in, attr_pool);
        else
            rv = apr_file_dup2(attr->parent_in, parent_in, attr_pool);
    }

    return rv;
}

APR_DECLARE(apr_status_t) apr_procattr_child_out_set(apr_procattr_t *attr,
                                                   apr_file_t *child_out,
                                                   apr_file_t *parent_out)
{
    apr_status_t rv = APR_SUCCESS;

    if (child_out) {
        if (attr->child_out == NULL)
            rv = apr_file_dup(&attr->child_out, child_out, attr_pool);
        else
            rv = apr_file_dup2(attr->child_out, child_out, attr_pool);

        if (rv == APR_SUCCESS)
            rv = apr_file_inherit_set(attr->child_out);
    }

    if (parent_out && rv == APR_SUCCESS) {
        if (attr->parent_out == NULL)
            rv = apr_file_dup(&attr->parent_out, parent_out, attr_pool);
        else
            rv = apr_file_dup2(attr->parent_out, parent_out, attr_pool);
    }

    return rv;
}

APR_DECLARE(apr_status_t) apr_procattr_child_err_set(apr_procattr_t *attr,
                                                   apr_file_t *child_err,
                                                   apr_file_t *parent_err)
{
    apr_status_t rv = APR_SUCCESS;

    if (child_err) {
        if (attr->child_err == NULL)
            rv = apr_file_dup(&attr->child_err, child_err, attr_pool);
        else
            rv = apr_file_dup2(attr->child_err, child_err, attr_pool);

        if (rv == APR_SUCCESS)
            rv = apr_file_inherit_set(attr->child_err);
    }

    if (parent_err && rv == APR_SUCCESS) {
        if (attr->parent_err == NULL)
            rv = apr_file_dup(&attr->parent_err, parent_err, attr_pool);
        else
            rv = apr_file_dup2(attr->parent_err, parent_err, attr_pool);
    }

    return rv;
}

APR_DECLARE(apr_status_t) apr_procattr_dir_set(apr_procattr_t *attr,
                                              const char *dir) 
{
    /* curr dir must be in native format, there are all sorts of bugs in
     * the NT library loading code that flunk the '/' parsing test.
     */
    return apr_filepath_merge(&attr->currdir, NULL, dir, 
                              APR_FILEPATH_NATIVE, attr->pool);
}

APR_DECLARE(apr_status_t) apr_procattr_cmdtype_set(apr_procattr_t *attr,
                                                  apr_cmdtype_e cmd) 
{
    attr->cmdtype = cmd;
    return APR_SUCCESS;
}

APR_DECLARE(apr_status_t) apr_procattr_detach_set(apr_procattr_t *attr,
                                                 apr_int32_t det) 
{
    attr->detached = det;
    return APR_SUCCESS;
}

static const char* has_space(const char *str)
{
    const char *ch;
    for (ch = str; *ch; ++ch) {
        if (apr_isspace(*ch)) {
            return ch;
        }
    }
    return NULL;
}

static char *apr_caret_escape_args(apr_pool_t *p, const char *str)
{
    char *cmd;
    unsigned char *d;
    const unsigned char *s;

    cmd = apr_palloc(p, 2 * strlen(str) + 1);	/* Be safe */
    d = (unsigned char *)cmd;
    s = (const unsigned char *)str;
    for (; *s; ++s) {

        /* 
         * Newlines to Win32/OS2 CreateProcess() are ill advised.
         * Convert them to spaces since they are effectively white
         * space to most applications
         */
	if (*s == '\r' || *s == '\n') {
	    *d++ = ' ';
            continue;
	}

	if (IS_SHCHAR(*s)) {
	    *d++ = '^';
	}
	*d++ = *s;
    }
    *d = '\0';

    return cmd;
}

APR_DECLARE(apr_status_t) apr_proc_create(apr_proc_t *new,
                                          const char *progname,
                                          const char * const *args,
                                          const char * const *env,
                                          apr_procattr_t *attr,
                                          apr_pool_t *pool)
{
    apr_status_t rv;
    apr_size_t i;
    const char *argv0;
    char *cmdline;
    char *pEnvBlock;
    PROCESS_INFORMATION pi;
    DWORD dwCreationFlags = 0;

    new->in = attr->parent_in;
    new->out = attr->parent_out;
    new->err = attr->parent_err;

    if (attr->detached) {
        /* If we are creating ourselves detached, Then we should hide the
         * window we are starting in.  And we had better redfine our
         * handles for STDIN, STDOUT, and STDERR. Do not set the
         * detached attribute for Win9x. We have found that Win9x does
         * not manage the stdio handles properly when running old 16
         * bit executables if the detached attribute is set.
         */
        if (apr_os_level >= APR_WIN_NT) {
            /* 
             * XXX DETACHED_PROCESS won't on Win9x at all; on NT/W2K 
             * 16 bit executables fail (MS KB: Q150956)
             */
            dwCreationFlags |= DETACHED_PROCESS;
        }
    }

    /* progname must be unquoted, in native format, as there are all sorts 
     * of bugs in the NT library loader code that fault when parsing '/'.
     * XXX progname must be NULL if this is a 16 bit app running in WOW
     */
    if (progname[0] == '\"') {
        progname = apr_pstrndup(pool, progname + 1, strlen(progname) - 2);
    }

    if (attr->cmdtype == APR_PROGRAM || attr->cmdtype == APR_PROGRAM_ENV) {
        char *fullpath = NULL;
        if ((rv = apr_filepath_merge(&fullpath, attr->currdir, progname, 
                                     APR_FILEPATH_NATIVE, pool)) != APR_SUCCESS) {
            return rv;
        }
        progname = fullpath;
    } 
    else {
        /* Do not fail if the path isn't parseable for APR_PROGRAM_PATH
         * or APR_SHELLCMD.  We only invoke apr_filepath_merge (with no
         * left hand side expression) in order to correct the path slash
         * delimiters.  But the filename doesn't need to be in the CWD,
         * nor does it need to be a filename at all (it could be a
         * built-in shell command.)
         */
        char *fullpath = NULL;
        if ((rv = apr_filepath_merge(&fullpath, "", progname, 
                                     APR_FILEPATH_NATIVE, pool)) == APR_SUCCESS) {
            progname = fullpath;
        }        
    }

    if (has_space(progname)) {
        argv0 = apr_pstrcat(pool, "\"", progname, "\"", NULL);
    }
    else {
        argv0 = progname;
    }

    /* Handle the args, seperate from argv0 */
    cmdline = "";
    for (i = 1; args && args[i]; ++i) {
        if (has_space(args[i])) {
            cmdline = apr_pstrcat(pool, cmdline, " \"", args[i], "\"", NULL);
        }
        else {
            cmdline = apr_pstrcat(pool, cmdline, " ", args[i], NULL);
        }
    }

#ifndef _WIN32_WCE
    if (attr->cmdtype == APR_SHELLCMD) {
        char *shellcmd = getenv("COMSPEC");
        if (!shellcmd) {
            return APR_EINVAL;
        }
        if (shellcmd[0] == '"') {
            progname = apr_pstrndup(pool, shellcmd + 1, strlen(shellcmd) - 2);
        }
        else {
            progname = shellcmd;
            if (has_space(shellcmd)) {
                shellcmd = apr_pstrcat(pool, "\"", shellcmd, "\"", NULL);
            }
        }
        /* Command.com does not support a quoted command, while cmd.exe demands one.
         */
        i = strlen(progname);
        if (i >= 11 && strcasecmp(progname + i - 11, "command.com") == 0) {
            cmdline = apr_pstrcat(pool, shellcmd, " /C ", argv0, cmdline, NULL);
        }
        else {
            cmdline = apr_pstrcat(pool, shellcmd, " /C \"", argv0, cmdline, "\"", NULL);
        }
    } 
    else 
#endif
    {
        /* Win32 is _different_ than unix.  While unix will find the given
         * program since it's already chdir'ed, Win32 cannot since the parent
         * attempts to open the program with it's own path.
         * ###: This solution isn't much better - it may defeat path searching
         * when the path search was desired.  Open to further discussion.
         */
        i = strlen(progname);
        if (i >= 4 && (strcasecmp(progname + i - 4, ".bat") == 0
                    || strcasecmp(progname + i - 4, ".cmd") == 0))
        {
            char *shellcmd = getenv("COMSPEC");
            if (!shellcmd) {
                return APR_EINVAL;
            }
            if (shellcmd[0] == '"') {
                progname = apr_pstrndup(pool, shellcmd + 1, strlen(shellcmd) - 2);
            }
            else {
                progname = shellcmd;
                if (has_space(shellcmd)) {
                    shellcmd = apr_pstrcat(pool, "\"", shellcmd, "\"", NULL);
                }
            }
            i = strlen(progname);
            if (i >= 11 && strcasecmp(progname + i - 11, "command.com") == 0) {
                /* XXX: Still insecure - need doubled-quotes on each individual
                 * arg of cmdline.  Suspect we need to postpone cmdline parsing
                 * until this moment in all four code paths, with some flags
                 * to toggle 'which flavor' is needed.
                 */
                cmdline = apr_pstrcat(pool, shellcmd, " /C ", argv0, cmdline, NULL);
            }
            else {
                /* We must protect the cmdline args from any interpolation - this
                 * is not a shellcmd, and the source of argv[] is untrusted.
                 * Notice we escape ALL the cmdline args, including the quotes
                 * around the individual args themselves.  No sense in allowing
                 * the shift-state to be toggled, and the application will 
                 * not see the caret escapes.
                 */
                cmdline = apr_caret_escape_args(pool, cmdline);
                /*
                 * Our app name must always be quoted so the quotes surrounding
                 * the entire /c "command args" are unambigious.
                 */
                if (*argv0 != '"') {
                    cmdline = apr_pstrcat(pool, shellcmd, " /C \"\"", argv0, "\"", cmdline, "\"", NULL);
                }
                else {
                    cmdline = apr_pstrcat(pool, shellcmd, " /C \"", argv0, cmdline, "\"", NULL);
                }
            }
        }
        else {
            /* A simple command we are directly invoking.  Do not pass
             * the first arg to CreateProc() for APR_PROGRAM_PATH
             * invocation, since it would need to be a literal and
             * complete file path.  That is; "c:\bin\aprtest.exe"
             * would succeed, but "c:\bin\aprtest" or "aprtest.exe"
             * can fail.
             */
            cmdline = apr_pstrcat(pool, argv0, cmdline, NULL);

            if (attr->cmdtype == APR_PROGRAM_PATH) {
                progname = NULL;
            }
        }
    }

    if (!env || attr->cmdtype == APR_PROGRAM_ENV) 
        pEnvBlock = NULL;
    else {
        apr_size_t iEnvBlockLen;
        /*
         * Win32's CreateProcess call requires that the environment
         * be passed in an environment block, a null terminated block of
         * null terminated strings.
         */  
        i = 0;
        iEnvBlockLen = 1;
        while (env[i]) {
            iEnvBlockLen += strlen(env[i]) + 1;
            i++;
        }
        if (!i) 
            ++iEnvBlockLen;

#if APR_HAS_UNICODE_FS
        IF_WIN_OS_IS_UNICODE
        {
            apr_wchar_t *pNext;
            pEnvBlock = (char *)apr_palloc(pool, iEnvBlockLen * 2);
            dwCreationFlags |= CREATE_UNICODE_ENVIRONMENT;

            i = 0;
            pNext = (apr_wchar_t*)pEnvBlock;
            while (env[i]) {
                apr_size_t in = strlen(env[i]) + 1;
                if ((rv = apr_conv_utf8_to_ucs2(env[i], &in, 
                                                pNext, &iEnvBlockLen)) 
                        != APR_SUCCESS) {
                    return rv;
                }
                pNext = wcschr(pNext, L'\0') + 1;
                i++;
            }
	    if (!i)
                *(pNext++) = L'\0';
	    *pNext = L'\0';
        }
#endif /* APR_HAS_UNICODE_FS */
#if APR_HAS_ANSI_FS
        ELSE_WIN_OS_IS_ANSI
        {
            char *pNext;
            pEnvBlock = (char *)apr_palloc(pool, iEnvBlockLen);
    
            i = 0;
            pNext = pEnvBlock;
            while (env[i]) {
                strcpy(pNext, env[i]);
                pNext = strchr(pNext, '\0') + 1;
                i++;
            }
	    if (!i)
                *(pNext++) = '\0';
	    *pNext = '\0';
        }
#endif /* APR_HAS_ANSI_FS */
    } 

    new->invoked = cmdline;

#if APR_HAS_UNICODE_FS
    IF_WIN_OS_IS_UNICODE
    {
        STARTUPINFOW si;
        apr_wchar_t *wprg = NULL;
        apr_wchar_t *wcmd = NULL;
        apr_wchar_t *wcwd = NULL;

        if (progname) {
            apr_size_t nprg = strlen(progname) + 1;
            apr_size_t nwprg = nprg + 6;
            wprg = apr_palloc(pool, nwprg * sizeof(wprg[0]));
            if ((rv = apr_conv_utf8_to_ucs2(progname, &nprg, wprg, &nwprg))
                   != APR_SUCCESS) {
                return rv;
            }
        }

        if (cmdline) {
            apr_size_t ncmd = strlen(cmdline) + 1;
            apr_size_t nwcmd = ncmd;
            wcmd = apr_palloc(pool, nwcmd * sizeof(wcmd[0]));
            if ((rv = apr_conv_utf8_to_ucs2(cmdline, &ncmd, wcmd, &nwcmd))
                    != APR_SUCCESS) {
                return rv;
            }
        }

        if (attr->currdir)
        {
            apr_size_t ncwd = strlen(attr->currdir) + 1;
            apr_size_t nwcwd = ncwd;
            wcwd = apr_palloc(pool, ncwd * sizeof(wcwd[0]));
            if ((rv = apr_conv_utf8_to_ucs2(attr->currdir, &ncwd, 
                                            wcwd, &nwcwd))
                    != APR_SUCCESS) {
                return rv;
            }
        }

        memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        if (attr->detached) {
            si.dwFlags |= STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
        }
#ifndef _WIN32_WCE
        if ((attr->child_in && attr->child_in->filehand)
            || (attr->child_out && attr->child_out->filehand)
            || (attr->child_err && attr->child_err->filehand))
        {
            si.dwFlags |= STARTF_USESTDHANDLES;
            if (attr->child_in)
                si.hStdInput = attr->child_in->filehand;
            if (attr->child_out)
                si.hStdOutput = attr->child_out->filehand;
            if (attr->child_err)
                si.hStdError = attr->child_err->filehand;
        }
        rv = CreateProcessW(wprg, wcmd,        /* Executable & Command line */
                            NULL, NULL,        /* Proc & thread security attributes */
                            TRUE,              /* Inherit handles */
                            dwCreationFlags,   /* Creation flags */
                            pEnvBlock,         /* Environment block */
                            wcwd,              /* Current directory name */
                            &si, &pi);
#else
        rv = CreateProcessW(wprg, wcmd,        /* Executable & Command line */
                            NULL, NULL,        /* Proc & thread security attributes */
                            FALSE,             /* must be 0 */
                            dwCreationFlags,   /* Creation flags */
                            NULL,              /* Environment block must be NULL */
                            NULL,              /* Current directory name must be NULL*/
                            NULL,              /* STARTUPINFO not supported */
                            &pi);
#endif
    }
#endif /* APR_HAS_UNICODE_FS */
#if APR_HAS_ANSI_FS
    ELSE_WIN_OS_IS_ANSI
    {
        STARTUPINFOA si;
        memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        if (attr->detached) {
            si.dwFlags |= STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
        }
        if ((attr->child_in && attr->child_in->filehand)
            || (attr->child_out && attr->child_out->filehand)
            || (attr->child_err && attr->child_err->filehand))
        {
            si.dwFlags |= STARTF_USESTDHANDLES;
            if (attr->child_in)
                si.hStdInput = attr->child_in->filehand;
            if (attr->child_out)
                si.hStdOutput = attr->child_out->filehand;
            if (attr->child_err)
                si.hStdError = attr->child_err->filehand;
        }

        rv = CreateProcessA(progname, cmdline, /* Command line */
                            NULL, NULL,        /* Proc & thread security attributes */
                            TRUE,              /* Inherit handles */
                            dwCreationFlags,   /* Creation flags */
                            pEnvBlock,         /* Environment block */
                            attr->currdir,     /* Current directory name */
                            &si, &pi);
    }
#endif /* APR_HAS_ANSI_FS */

    /* Check CreateProcess result 
     */
    if (!rv)
        return apr_get_os_error();

    /* XXX Orphaned handle warning - no fix due to broken apr_proc_t api.
     */
    new->hproc = pi.hProcess;
    new->pid = pi.dwProcessId;

    if (attr->child_in) {
        apr_file_close(attr->child_in);
    }
    if (attr->child_out) {
        apr_file_close(attr->child_out);
    }
    if (attr->child_err) {
        apr_file_close(attr->child_err);
    }
    CloseHandle(pi.hThread);

    return APR_SUCCESS;
}

APR_DECLARE(apr_status_t) apr_proc_wait(apr_proc_t *proc,
                                        int *exitcode, apr_exit_why_e *exitwhy,
                                        apr_wait_how_e waithow)
{
    DWORD stat;
    DWORD time;

    if (!proc)
        return APR_ENOPROC;

    if (waithow == APR_WAIT)
        time = INFINITE;
    else
        time = 0;

    if ((stat = WaitForSingleObject(proc->hproc, time)) == WAIT_OBJECT_0) {
        if (GetExitCodeProcess(proc->hproc, &stat)) {
            if (exitcode)
                *exitcode = (apr_wait_t)stat;
            CloseHandle(proc->hproc);
            proc->hproc = NULL;
            return APR_CHILD_DONE;
        }
    }
    else if (stat == WAIT_TIMEOUT) {
        return APR_CHILD_NOTDONE;
    }
    return apr_get_os_error();
}
