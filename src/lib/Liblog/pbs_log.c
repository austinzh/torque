/*
*         OpenPBS (Portable Batch System) v2.3 Software License
*
* Copyright (c) 1999-2000 Veridian Information Solutions, Inc.
* All rights reserved.
*
* ---------------------------------------------------------------------------
* For a license to use or redistribute the OpenPBS software under conditions
* other than those described below, or to purchase support for this software,
* please contact Veridian Systems, PBS Products Department ("Licensor") at:
*
*    www.OpenPBS.org  +1 650 967-4675                  sales@OpenPBS.org
*                        877 902-4PBS (US toll-free)
* ---------------------------------------------------------------------------
*
* This license covers use of the OpenPBS v2.3 software (the "Software") at
* your site or location, and, for certain users, redistribution of the
* Software to other sites and locations.  Use and redistribution of
* OpenPBS v2.3 in source and binary forms, with or without modification,
* are permitted provided that all of the following conditions are met.
* After December 31, 2001, only conditions 3-6 must be met:
*
* 1. Commercial and/or non-commercial use of the Software is permitted
*    provided a current software registration is on file at www.OpenPBS.org.
*    If use of this software contributes to a publication, product, or
*    service, proper attribution must be given; see www.OpenPBS.org/credit.html
*
* 2. Redistribution in any form is only permitted for non-commercial,
*    non-profit purposes.  There can be no charge for the Software or any
*    software incorporating the Software.  Further, there can be no
*    expectation of revenue generated as a consequence of redistributing
*    the Software.
*
* 3. Any Redistribution of source code must retain the above copyright notice
*    and the acknowledgment contained in paragraph 6, this list of conditions
*    and the disclaimer contained in paragraph 7.
*
* 4. Any Redistribution in binary form must reproduce the above copyright
*    notice and the acknowledgment contained in paragraph 6, this list of
*    conditions and the disclaimer contained in paragraph 7 in the
*    documentation and/or other materials provided with the distribution.
*
* 5. Redistributions in any form must be accompanied by information on how to
*    obtain complete source code for the OpenPBS software and any
*    modifications and/or additions to the OpenPBS software.  The source code
*    must either be included in the distribution or be available for no more
*    than the cost of distribution plus a nominal fee, and all modifications
*    and additions to the Software must be freely redistributable by any party
*    (including Licensor) without restriction.
*
* 6. All advertising materials mentioning features or use of the Software must
*    display the following acknowledgment:
*
*     "This product includes software developed by NASA Ames Research Center,
*     Lawrence Livermore National Laboratory, and Veridian Information
*     Solutions, Inc.
*     Visit www.OpenPBS.org for OpenPBS software support,
*     products, and information."
*
* 7. DISCLAIMER OF WARRANTY
*
* THIS SOFTWARE IS PROVIDED "AS IS" WITHOUT WARRANTY OF ANY KIND. ANY EXPRESS
* OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
* OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, AND NON-INFRINGEMENT
* ARE EXPRESSLY DISCLAIMED.
*
* IN NO EVENT SHALL VERIDIAN CORPORATION, ITS AFFILIATED COMPANIES, OR THE
* U.S. GOVERNMENT OR ANY OF ITS AGENCIES BE LIABLE FOR ANY DIRECT OR INDIRECT,
* INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
* LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
* OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
* LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
* NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
* EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*
* This license will be governed by the laws of the Commonwealth of Virginia,
* without reference to its choice of law rules.
*/
/*
 * pbs_log.c - contains functions to log error and event messages to
 * the log file.
 *
 * Functions included are:
 * log_open()
 * log_err()
 * log_ext()
 * log_record()
 * log_close()
 * log_roll()
 * log_size()
 */

#include <pbs_config.h>   /* the master config generated by configure */
#include "pbs_log.h"

#include "portability.h"
#include "pbs_error.h"

#include <sys/param.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <limits.h>
#include <time.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/time.h>

#include "log.h"
#if SYSLOG
#include <syslog.h>
#endif

#include <execinfo.h> /* backtrace information */
#include<arpa/inet.h>
#include<netdb.h>

void job_log_close(int msg);

/* Global Data */
char log_buffer[LOG_BUF_SIZE];
char log_directory[_POSIX_PATH_MAX/2];
char job_log_directory[_POSIX_PATH_MAX/2];
char log_host_port[1024];
char log_host[1024];
char log_suffix[1024];

char *msg_daemonname = (char *)"unset";

/* Local Data */

static int      log_auto_switch = 0;
static int      log_open_day;
static FILE     *logfile;  /* open stream for log file */
static char     *logpath = NULL;
static volatile int  log_opened = 0;
#if SYSLOG
static int      syslogopen = 0;
#endif /* SYSLOG */

pthread_mutex_t log_mutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

/* variables for job logging */
static int      job_log_auto_switch = 0;
static int      joblog_open_day;
static FILE     *joblogfile;  /* open stream for log file */
static char     *joblogpath = NULL;
static volatile int  job_log_opened = 0;

pthread_mutex_t job_log_mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * the order of these names MUST match the defintions of
 * PBS_EVENTCLASS_* in log.h
 */
static const char *class_names[] =
  {
  "n/a",
  "Svr",
  "Que",
  "Job",
  "Req",
  "Fil",
  "Act",
  "node",
  "trqauthd"
  };

/* External functions called */

/* local prototypes */
const char *log_get_severity_string(int);


/*
 * mk_log_name - make the log name used by MOM
 * based on the date: yyyymmdd
 */

static char *mk_log_name(

  char *pbuf)     /* O (minsize=1024) */

  {
  struct tm *ptm;
  struct tm  tmpPtm;
  time_t     time_now = time(NULL);
  memset(&tmpPtm, 0, sizeof(struct tm));

  ptm = localtime_r(&time_now,&tmpPtm);

  if (log_suffix[0] != '\0')
    {
    if (!strcasecmp(log_suffix, "%h"))
      {
      snprintf(pbuf, PATH_MAX, "%s/%04d%02d%02d.%s",
              log_directory,
              ptm->tm_year + 1900,
              ptm->tm_mon + 1,
              ptm->tm_mday,
              (log_host[0] != '\0') ? log_host : "localhost");
      }
    else
      {
      snprintf(pbuf, PATH_MAX, "%s/%04d%02d%02d.%s",
              log_directory,
              ptm->tm_year + 1900,
              ptm->tm_mon + 1,
              ptm->tm_mday,
              log_suffix);
      }
    }
  else
    {
    snprintf(pbuf, PATH_MAX, "%s/%04d%02d%02d",
            log_directory,
            ptm->tm_year + 1900,
            ptm->tm_mon + 1,
            ptm->tm_mday);
    }

  log_open_day = ptm->tm_yday; /* Julian date log opened */

  return(pbuf);
  }  /* END mk_log_name() */



/*
 * mk_job_log_name - make the log name used by MOM
 * based on the date: yyyymmdd
 */

static char *mk_job_log_name(

  char *pbuf,
  int   buf_size)

  {
  struct tm *ptm;
  struct tm  tmpPtm;
  time_t time_now = time(NULL);

  ptm = localtime_r(&time_now,&tmpPtm);

  if (log_suffix[0] != '\0')
    {
    if (!strcasecmp(log_suffix, "%h"))
      {
      snprintf(pbuf, buf_size, "%s/%04d%02d%02d.%s",
              job_log_directory,
              ptm->tm_year + 1900,
              ptm->tm_mon + 1,
              ptm->tm_mday,
              (log_host[0] != '\0') ? log_host : "localhost");
      }
    else
      {
      snprintf(pbuf, buf_size, "%s/%04d%02d%02d.%s",
              job_log_directory,
              ptm->tm_year + 1900,
              ptm->tm_mon + 1,
              ptm->tm_mday,
              log_suffix);
      }
    }
  else
    {
    snprintf(pbuf, buf_size, "%s/%04d%02d%02d",
            job_log_directory,
            ptm->tm_year + 1900,
            ptm->tm_mon + 1,
            ptm->tm_mday);
    }

  joblog_open_day = ptm->tm_yday; /* Julian date log opened */

  return(pbuf);
  }  /* END mk_job_log_name() */




int log_init(

  const char *suffix,    /* I (optional) */
  const char *hostname)  /* I (optional) */

  {
  if (suffix != NULL)
    snprintf(log_suffix, sizeof(log_suffix), "%s", suffix);

  if (hostname != NULL)
    snprintf(log_host, sizeof(log_host), "%s", hostname);

  return(PBSE_NONE);
  }  /* END log_init() */




/*
 * log_open() - open the log file for append.
 *
 * Opens a (new) log file.
 * If a log file is already open, and the new file is successfully opened,
 * the old file is closed.  Otherwise the old file is left open.
 */

int log_open(

  char *filename,  /* abs filename or NULL */
  char *directory) /* normal log directory */

  {
  char  buf[PATH_MAX];
  char  buf2[1024];
  int   fds;

  if (log_opened > 0)
    {
    return(-1); /* already open */
    }

  if (log_directory != directory)  /* some calls pass in log_directory */
    {
    snprintf(log_directory, (_POSIX_PATH_MAX) / 2 - 1, "%s", directory);
    }

  if ((filename == NULL) || (*filename == '\0'))
    {
    filename = mk_log_name(buf);

    log_auto_switch = 1;
    }
  else if (*filename != '/')
    {
    return(-1); /* must be absolute path */
    }

  if ((fds = open(filename, O_CREAT | O_WRONLY | O_APPEND, 0644)) < 0)
    {
    log_opened = -1; /* note that open failed */

    return(-1);
    }

  if (fds < 3)
    {
    log_opened = fcntl(fds, F_DUPFD, 3); /* overload variable */

    if (log_opened < 0)
      {
      close(fds);
      return(-1);
      }

    close(fds);

    fds = log_opened;
    }

  /* save the path of the last opened logfile for log_roll */
  if (logpath != filename)
    {
    if (logpath != NULL)
      free(logpath);

    logpath = strdup(filename);
    }

  logfile = fdopen(fds, "a");

  if (logfile == NULL)
    {
    log_opened = -1;
    return(-1);
    }

  setvbuf(logfile, NULL, _IOLBF, 0); /* set line buffering */

  log_opened = 1;   /* note that file is open */

  pthread_mutex_unlock(&log_mutex);

  if (log_host_port[0])
    snprintf(buf2, sizeof(buf2), "Log opened at %s", log_host_port);
  else
    snprintf(buf2, sizeof(buf2), "Log opened");
  
  if (log_host_port[0])
    snprintf(buf2, sizeof(buf2), "Log opened at %s", log_host_port);
  else
    snprintf(buf2, sizeof(buf2), "Log opened");
  
  log_record(
    PBSEVENT_SYSTEM,
    PBS_EVENTCLASS_SERVER,
    "Log",
    buf2);

  pthread_mutex_lock(&log_mutex);

  return(0);
  }  /* END log_open() */



/*
 * job_log_open() - open the log file for append.
 *
 * Opens a (new) log file.
 * If a log file is already open, and the new file is successfully opened,
 * the old file is closed.  Otherwise the old file is left open.
 */

int job_log_open(

  char *filename,  /* abs filename or NULL */
  char *directory) /* normal log directory */

  {
  char  buf[_POSIX_PATH_MAX];
  char  err_log[256];
  int   fds;

  if (job_log_opened > 0)
    {
    return(1); /* already open */
    }

  if (job_log_directory != directory)  /* some calls pass in job_log_directory */
    {
    snprintf(job_log_directory, (_POSIX_PATH_MAX) / 2 - 1, "%s", directory);
    }

  if ((filename == NULL) || (*filename == '\0'))
    {
    filename = mk_job_log_name(buf, _POSIX_PATH_MAX);

    job_log_auto_switch = 1;
    }
  else if (*filename != '/')
    {
    sprintf(err_log, "must use absolute file path: %s", filename);
    log_err(-1, __func__, err_log);
    return(-1); /* must be absolute path */
    }

  if ((fds = open(filename, O_CREAT | O_WRONLY | O_APPEND, 0644)) < 0)
    {
    job_log_opened = -1; /* note that open failed */

    sprintf(err_log, "could not open %s ", filename);
    log_err(errno, __func__, err_log);
    return(-1);
    }

  if (fds < 3)
    {
    job_log_opened = fcntl(fds, F_DUPFD, 3); /* overload variable */

    if (job_log_opened < 0)
      {
      close(fds);
      log_err(errno, __func__, "failed to dup job log file descriptor");
      return(-1);
      }

    close(fds);

    fds = job_log_opened;
    }

  /* save the path of the last opened joblogfile for log_roll */
  if (joblogpath != filename)
    {
    if (joblogpath != NULL)
      free(joblogpath);

    joblogpath = strdup(filename);
    }

  joblogfile = fdopen(fds, "a");

  setvbuf(joblogfile, NULL, _IOLBF, 0); /* set line buffering */

  job_log_opened = 1;   /* note that file is open */

  return(0);
  }  /* END job_log_open() */




/*
 * log_err - log an internal error
 * The error is recorded to the pbs log file and to syslogd if it is
 * available.  If the error file has not been opened and if syslog is
 * not defined, then the console is opened.
 */

void log_err(

  int         errnum,  /* I (errno or PBSErrno) */
  const char *routine, /* I */
  const char *text)    /* I */

  {
  log_ext(errnum,routine,text,LOG_ERR);

  return;
  }  /* END log_err() */




/*
 *  log_ext (log extended) - log message to syslog (if available) and to the TORQUE log.
 *
 *  The error is recorded to the TORQUE log file and to syslogd if it is
 *  available.  If the error file has not been opened and if syslog is
 *  not defined, then the console is opened.

 *  Note that this function differs from log_err in that you can specify a severity
 *  level that will accompany the message in the syslog (see 'manpage syslog' for a list
 *  of severity/priority levels). This function, is in fact, used by log_err--it just uses
 *  a severity level of LOG_ERR
 */

void log_ext(

  int         errnum,   /* I (errno or PBSErrno) */
  const char *routine,  /* I */
  const char *text,     /* I */
  int         severity) /* I */

  {
  char  buf[LOG_BUF_SIZE];

  char *EPtr = NULL;

  char  EBuf[1024];

  char  tmpLine[2048];

  const char *SeverityText = NULL;

  tmpLine[0] = '\0';

  EBuf[0] = '\0';

  if (errnum == -1)
    {
    buf[0] = '\0';
    }
  else
    {
    /* NOTE:  some strerror() routines return "Unknown error X" w/bad errno */

    if (errnum >= 15000)
      {
      EPtr = pbse_to_txt(errnum);
      }
    else
      {
      EPtr = strerror(errnum);
      }

    if (EPtr == NULL)
      {
      sprintf(EBuf, "unexpected error %d",
              errnum);

      EPtr = EBuf;
      }

    sprintf(tmpLine,"%s (%d) in ", 
            EPtr,
            errnum);
    }

  SeverityText = log_get_severity_string(severity);

  snprintf(buf,sizeof(buf),"%s::%s%s, %s",
    SeverityText,
    tmpLine,
    routine,
    text);

  buf[LOG_BUF_SIZE - 1] = '\0';

  pthread_mutex_lock(&log_mutex);

  if (log_opened == 0)
    {
#if !SYSLOG
    log_open("/dev/console", log_directory);
#endif /* not SYSLOG */
    }

  if (isatty(2))
    {
    fprintf(stderr, "%s: %s\n",
            msg_daemonname,
            buf);
    }
    
  if (log_opened > 0)
    {
    pthread_mutex_unlock(&log_mutex);

    log_record(
      PBSEVENT_ERROR | PBSEVENT_FORCE,
      PBS_EVENTCLASS_SERVER,
      msg_daemonname,
      buf);
    }
  else
    pthread_mutex_unlock(&log_mutex);

#if SYSLOG
  if (syslogopen == 0)
    {
    openlog(msg_daemonname, LOG_NOWAIT, LOG_DAEMON);

    syslogopen = 1;
    }

  syslog(severity|LOG_DAEMON,"%s",buf);

#endif /* SYSLOG */

  return;
  }  /* END log_ext() */



/**
 * Returns a string to represent the syslog severity-level given.
 */

const char *log_get_severity_string(

  int severity)

  {
  const char *result = NULL;

  /* We can't just index into an array to get the strings, as we don't always
   * have control over what the value of the LOG_* consts are */

  switch (severity)
    {
    case LOG_EMERG:
      result = "LOG_EMERGENCY";
      break;
    
    case LOG_ALERT:
      result = "LOG_ALERT";
      break;

    case LOG_CRIT:
      result = "LOG_CRITICAL";
      break;

    case LOG_ERR:
      result = "LOG_ERROR";
      break;

    case LOG_WARNING:
      result = "LOG_WARNING";
      break;

    case LOG_NOTICE:
      result = "LOG_NOTICE";
      break;

    case LOG_DEBUG:
      result = "LOG_DEBUG";
      break;

    case LOG_INFO:
    default:
      result = "LOG_INFO";
      break;
    }

  return(result);
  }  /* END log_get_severity_string() */


/* record job information of completed job to job log */
int log_job_record(const char *buf)
  {
  struct tm *ptm;
  struct tm tmpPtm;
  time_t now;

  now = time((time_t *)0);
  ptm = localtime_r(&now,&tmpPtm);

  pthread_mutex_lock(&job_log_mutex);

  /* do we need to switch the log to the new day? */
  if (job_log_auto_switch && (ptm->tm_yday != joblog_open_day))
    {
    job_log_close(1);

    job_log_open(NULL, job_log_directory);

    if (job_log_opened < 1)
      {
      log_err(-1, __func__, "job_log_opened < 1");
      pthread_mutex_unlock(&job_log_mutex);
      return(-1);
      }
    }

  fprintf(joblogfile, "%s\n", buf);
  fflush(joblogfile);
  pthread_mutex_unlock(&job_log_mutex);

  return(0);
  }

/*
 * See if the log is open. In client commands this will return false.
 */

bool log_available(int eventtype)
  {
#if SYSLOG
  if(eventtype&PBSEVENT_SYSLOG)
    return true;
#endif
  if(log_opened < 1)
    return true;
  return false;
  }


/*
 * log_record - log a message to the log file
 * The log file must have been opened by log_open().
 *
 * NOTE:  do not use in pbs_mom spawned children - does not write to syslog!!!
 *
 * The caller should ensure proper formating of the message if "text"
 * is to contain "continuation lines".
 */

void log_record(

  int         eventtype,  /* I */
  int         objclass,   /* I */
  const char *objname,    /* I */
  const char *text)       /* I */

  {
  int tryagain = 2;
  time_t now;
  pid_t  thr_id = -1;

  struct tm *ptm;
  struct tm  tmpPtm;
  struct timeval mytime;
  int    rc = 0;
  FILE  *savlog;
  char  *start = NULL, *end = NULL;
  size_t nchars;
  int eventclass = 0;
  char time_formatted_str[64];

  thr_id = syscall(SYS_gettid);
  pthread_mutex_lock(&log_mutex);

#if SYSLOG
  if (eventtype & PBSEVENT_SYSLOG)
    {
    if (syslogopen == 0)
      {
      openlog(msg_daemonname, LOG_NOWAIT, LOG_DAEMON);

      syslogopen = 1;
      }

    syslog(LOG_ERR | LOG_DAEMON,"%s",text);
    }
#endif /* SYSLOG */

  if (log_opened < 1)
    {
    pthread_mutex_unlock(&log_mutex);
    return;
    }

  now = time((time_t *)0); /* get time for message */

  ptm = localtime_r(&now,&tmpPtm);
  
  /* mytime used to calculate milliseconds */
  
  gettimeofday(&mytime, NULL);
  
  int milliseconds = mytime.tv_usec/1000;

  /* Do we need to switch the log? */

  if (log_auto_switch && (ptm->tm_yday != log_open_day))
    {
    log_close(1);

    log_open(NULL, log_directory);

    if (log_opened < 1)
      {
      pthread_mutex_unlock(&log_mutex);
      return;
      }
    }
  
  time_formatted_str[0] = 0;    
  log_get_set_eventclass(&eventclass, GETV);
  if (eventclass == PBS_EVENTCLASS_TRQAUTHD)
    {
    log_format_trq_timestamp(time_formatted_str, sizeof(time_formatted_str));
    }

  /*
   * Looking for the newline characters and splitting the output message
   * on them.  Sequence "\r\n" is mapped to the single newline.
   */
  start = (char *)text;

  while (1)
    {
    for (end = start; *end != '\n' && *end != '\r' && *end != '\0'; end++)
      ;

    nchars = end - start;

    if (*end == '\r' && *(end + 1) == '\n')
      end++;

    while (tryagain)
      {
      if (eventclass != PBS_EVENTCLASS_TRQAUTHD)
        {
        rc = fprintf(logfile,
              "%02d/%02d/%04d %02d:%02d:%02d.%03d;%02d;%10.10s.%d;%s;%s;%s%.*s\n",
              ptm->tm_mon + 1,
              ptm->tm_mday,
              ptm->tm_year + 1900,
              ptm->tm_hour,
              ptm->tm_min,
              ptm->tm_sec,
			        milliseconds,
              (eventtype & ~PBSEVENT_FORCE),
              msg_daemonname,
              thr_id,
              class_names[objclass],
              objname,
              (text == start ? "" : "[continued]"),
              (int)nchars,
              start);
        }
      else
        {
        rc = fprintf(logfile,
              "%s %s%.*s\n",
              time_formatted_str,
              (text == start ? "" : "[continued]"),
              (int)nchars,
              start);
        }
      if ((rc < 0) &&
          (errno == EPIPE) &&
          (tryagain == 2))
        {
        /* the log file descriptor has been changed--it now points to a socket!
         * reopen log and leave the previous file descriptor alone--do not close it */

        log_opened = 0;
        log_open(NULL, log_directory);
        tryagain--;
        }
      else
        {
        tryagain = 0;
        }
      }

    if (rc < 0)
      break;

    if (*end == '\0')
      break;

    start = end + 1;
    }  /* END while (1) */

  fflush(logfile);

  if (rc < 0)
    {
    rc = errno;
    clearerr(logfile);
    savlog = logfile;
    logfile = fopen("/dev/console", "w");
    /* we need to add this check to make sure the disk isn't full so we don't segfault 
     * if we can't open this then we're going to have a nice surprise failure */
    if (logfile != NULL)
      {
      pthread_mutex_unlock(&log_mutex);
      log_err(rc, __func__, "PBS cannot write to its log");
      fclose(logfile);
      pthread_mutex_lock(&log_mutex);
      }

    logfile = savlog;
    }
  
  pthread_mutex_unlock(&log_mutex);

  return;
  }  /* END log_record() */




/*
 * log_close - close the current open log file
 */

void log_close(

  int msg)  /* BOOLEAN - write close message */

  {
  char buf[1024];
  if (log_opened == 1)
    {
    log_auto_switch = 0;

    if (msg)
      {
      if (log_host_port[0])
        snprintf(buf, sizeof(buf), "Log closed at %s", log_host_port);
      else
        snprintf(buf, sizeof(buf), "Log closed");
       
      pthread_mutex_unlock(&log_mutex);
      log_record(
        PBSEVENT_SYSTEM,
        PBS_EVENTCLASS_SERVER,
        "Log",
        buf);
      pthread_mutex_lock(&log_mutex);
      }

    fclose(logfile);

    log_opened = 0;
    }

#if SYSLOG

  if (syslogopen)
    closelog();

#endif /* SYSLOG */

  return;
  }  /* END log_close() */

/*
 * job_log_close - close the current open job log file
 */

void job_log_close(

  int msg)  /* BOOLEAN - write close message */

  {
  if (job_log_opened == 1)
    {
    job_log_auto_switch = 0;

    if (msg)
      {
      log_record(
        PBSEVENT_SYSTEM,
        PBS_EVENTCLASS_SERVER,
        "Log",
        "Log closed");
      }

    fclose(joblogfile);

    job_log_opened = 0;
    }

#if SYSLOG

  if (syslogopen)
    closelog();

#endif /* SYSLOG */

  return;
  }  /* END job_log_close() */




/**
 * log_remove_old - Removes log files older than a given time.
 * This function removes files from the given path if they were last modified after ExpireTime.
 *
 * Note that this function will skip over special filenames like '.' and '..'.
 *
 * @param DirPath (I)
 * @param ExpireTime (I, how old the file must be to be removed, in seconds)
 * if (Expiretime == 0), nothing will be removed
 * @return 0 on success, non-zero otherwise
 */

int log_remove_old(

  char  *DirPath,     /* I (path in which we're removing files)*/
  unsigned long ExpireTime)   /* I (how old the file must be to be removed, in seconds) */

  {
  char tmpPath[MAX_PATH_LEN];
 
  DIR *DirHandle;
  struct dirent *FileHandle;
  struct stat sbuf;
  unsigned long FMTime;
  unsigned long TTime;
  char  log_buf[LOCAL_LOG_BUF_SIZE];
 
  int IsDir = FALSE;
 
  /* check the input for an empty path */
  if ((DirPath == NULL) || (DirPath[0] == '\0'))
    {
    return(-1);
    }

  if (ExpireTime == 0)
    {
    return(0);
    }

  /* open directory for reading */
      
  DirHandle = opendir(DirPath);

  /* fail if path couldn't be opened */  
  if (DirHandle == (DIR *)NULL)
    {
    return(-1);
    }

  FileHandle = readdir(DirHandle);
  
  while (FileHandle != (struct dirent *)NULL)
    {    
    /* attempt to delete old files */

    if (!strcmp(FileHandle->d_name,".") ||
        !strcmp(FileHandle->d_name,".."))
      {
      /* skip special file names */

      FileHandle = readdir(DirHandle);

      continue;
      }

    snprintf(tmpPath,sizeof(tmpPath),"%s/%s",
      DirPath,
      FileHandle->d_name);

    if (stat(tmpPath,&sbuf) == -1) 
      {
      /* -1 is the failure value for stat */

      FMTime = 0;

      FileHandle = readdir(DirHandle);

      continue;
      }

    TTime = time((time_t *)NULL);
    /* set FMTime's value */
    FMTime = (unsigned long)sbuf.st_mtime;
    
    if ((IsDir == FALSE) &&
        (FMTime + ExpireTime) < TTime)
      {
      /* file is too old - delete it */
      snprintf(log_buf, LOCAL_LOG_BUF_SIZE, "Removing log %s - log age = %lu, Expire time = %lu", tmpPath, (TTime - FMTime), ExpireTime);
      log_err(-1, __func__, log_buf);

      remove(tmpPath);
      }

    FileHandle = readdir(DirHandle);
    } /* end file checking while loop */

  closedir(DirHandle);

  return(0);
  }  /* END log_remove_ld() */






void log_roll(

  int max_depth)

  {
  int i, suffix_size, file_buf_len, as;
  int err = 0;
  char *source  = NULL;
  char *dest    = NULL;
  
  pthread_mutex_lock(&log_mutex);

  if (!log_opened)
    {
    pthread_mutex_unlock(&log_mutex);
    return;
    }

  /* save value of log_auto_switch */
  as = log_auto_switch;

  log_close(1);

  /* find out how many characters the suffix could be. (save in suffix_size)
     start at 1 to account for the "."  */

  for (i = max_depth, suffix_size = 1;i > 0;suffix_size++, i /= 10)
    /* NO-OP */;

  /* allocate memory for rolling */

  file_buf_len = sizeof(char) * (strlen(logpath) + suffix_size + 1)
    /* NO-OP */;

  source = (char*)calloc(1, file_buf_len);

  dest   = (char*)calloc(1, file_buf_len);

  if ((source == NULL) || (dest == NULL))
    {
    err = errno;

    goto done_roll;
    }

  /* call unlink to delete logname.max_depth - it doesn't matter if it
     doesn't exist, so we'll ignore ENOENT */

  sprintf(dest, "%s.%d",
    logpath,
    max_depth);

  if ((unlink(dest) != 0) && (errno != ENOENT))
    {
    err = errno;
    goto done_roll;
    }

  /* logname.max_depth is gone, so roll the rest of the log files */

  for (i = max_depth - 1;i >= 0;i--)
    {
    if (i == 0)
      {
      strcpy(source, logpath);
      }
    else
      {
      sprintf(source, "%s.%d",
        logpath,
        i);
      }

    sprintf(dest, "%s.%d",
      logpath,
      i + 1);

    /* rename file if it exists */

    if ((rename(source, dest) != 0) && (errno != ENOENT))
      {
      err = errno;
      goto done_roll;
      }
    }    /* END for (i) */

done_roll:

  if (as)
    {
    log_open(NULL, log_directory);
    }
  else
    {
    log_open(logpath, log_directory);
    }

  pthread_mutex_unlock(&log_mutex);

  if (source != NULL)
    free(source);

  if (dest != NULL)
    free(dest);

  if (err != 0)
    {
    log_err(err, "log_roll", "error while rollng logs");
    }
  else
    {
    log_record(
      PBSEVENT_SYSTEM,
      PBS_EVENTCLASS_SERVER,
      "Log",
      "Log Rolled");
    }

  return;
  } /* END log_roll() */


void job_log_roll(

  int max_depth)

  {
  int i, suffix_size, file_buf_len, as;
  int err = 0;
  char *source  = NULL;
  char *dest    = NULL;
      
  pthread_mutex_lock(&job_log_mutex);

  if (!job_log_opened)
    {
    pthread_mutex_unlock(&job_log_mutex);
    return;
    }

  /* save value of job_log_auto_switch */

  as = job_log_auto_switch;

  job_log_close(1);

  /* find out how many characters the suffix could be. (save in suffix_size)
     start at 1 to account for the "."  */

  for (i = max_depth, suffix_size = 1;i > 0;suffix_size++, i /= 10)
    /* NO-OP */;

  /* allocate memory for rolling */

  file_buf_len = sizeof(char) * (strlen(joblogpath) + suffix_size + 1);

  source = (char*)calloc(1, file_buf_len);

  dest   = (char*)calloc(1, file_buf_len);

  if ((source == NULL) || (dest == NULL))
    {
    err = errno;

    goto done_job_roll;
    }

  /* call unlink to delete logname.max_depth - it doesn't matter if it
     doesn't exist, so we'll ignore ENOENT */

  sprintf(dest, "%s.%d",
    joblogpath,
    max_depth);


  if ((unlink(dest) != 0) && (errno != ENOENT))
    {
    err = errno;
    goto done_job_roll;
    }


  /* logname.max_depth is gone, so roll the rest of the log files */

  for (i = max_depth - 1;i >= 0;i--)
    {
    if (i == 0)
      {
      strcpy(source, joblogpath);
      }
    else
      {
      sprintf(source, "%s.%d",
        joblogpath,
        i);
      }

    sprintf(dest, "%s.%d",
      joblogpath,
      i + 1);

    /* rename file if it exists */

    if ((rename(source, dest) != 0) && (errno != ENOENT))
      {
      err = errno;
      goto done_job_roll;
      }
    }    /* END for (i) */

done_job_roll:

  if (as)
    {
    job_log_open(NULL, job_log_directory);
    }
  else
    {
    job_log_open(joblogpath, job_log_directory);
    }

  if (source != NULL)
    free(source);

  if (dest != NULL)
    free(dest);

  if (err != 0)
    {
    log_err(err, "log_roll", "error while rollng logs");
    }
  else
    {
    log_record(
      PBSEVENT_SYSTEM,
      PBS_EVENTCLASS_SERVER,
      "Job Log",
      "Job Log Rolled");
    }
    
  pthread_mutex_unlock(&job_log_mutex);

  return;
  } /* END job_log_roll() */






/* return size of log file in kilobytes */

long log_size(void)

  {
#if defined(HAVE_STRUCT_STAT64) && defined(HAVE_STAT64) && defined(LARGEFILE_WORKS)

  struct stat64 file_stat = {0};
#else

  struct stat file_stat;
#endif
  
  pthread_mutex_lock(&log_mutex);

#if defined(HAVE_STRUCT_STAT64) && defined(HAVE_STAT64) && defined(LARGEFILE_WORKS)

  if (log_opened && (fstat64(fileno(logfile), &file_stat) != 0))
#else
  if (log_opened && (fstat(fileno(logfile), &file_stat) != 0))
#endif
    {
    /* FAILURE */

    pthread_mutex_unlock(&log_mutex);
    /* log_err through log_ext will lock the log_mutex, so release log_mutex before calling log_err */
    log_err(errno, "log_size", "PBS cannot fstat logfile");

    return(0);
    }
  
  if (!log_opened) {
      pthread_mutex_unlock(&log_mutex);
      log_err(EAGAIN, "log_size", "PBS cannot find size of log file because logfile has not been opened");
      return(0);
  }

  pthread_mutex_unlock(&log_mutex);

  return(file_stat.st_size / 1024);
  }

/* return size of job log file in kilobytes */

long job_log_size(void)

  {
#if defined(HAVE_STRUCT_STAT64) && defined(HAVE_STAT64) && defined(LARGEFILE_WORKS)

  struct stat64 file_stat;
#else

  struct stat file_stat;
#endif
 
  memset(&file_stat, 0, sizeof(file_stat));
  pthread_mutex_lock(&job_log_mutex);

#if defined(HAVE_STRUCT_STAT64) && defined(HAVE_STAT64) && defined(LARGEFILE_WORKS)

  if (job_log_opened && (fstat64(fileno(joblogfile), &file_stat) != 0))
#else
  if (job_log_opened && (fstat(fileno(joblogfile), &file_stat) != 0))
#endif
    {
    /* FAILURE */

    log_err(errno, __func__, "PBS cannot fstat joblogfile");
    pthread_mutex_unlock(&job_log_mutex);

    return(0);
    }
  
  pthread_mutex_unlock(&job_log_mutex);

  return(file_stat.st_size / 1024);
  }


void print_trace(
    
  int socknum)

  {
  void  *array[10];
  int    size;
  char **meth_names;
  int    cntr;
  char   msg[120];
  char   meth_name[20];

  size = backtrace(array, 10);
  meth_names = backtrace_symbols(array, size);
  snprintf(meth_name, sizeof(meth_name), "pt - pos %d", socknum);
  snprintf(msg, sizeof(msg), "Obtained %d stack frames.\n", size);
  log_record(PBSEVENT_SYSTEM, PBS_EVENTCLASS_SERVER, meth_name, msg); 
  for (cntr = 0; cntr < size; cntr++)
    {
    log_record(PBSEVENT_SYSTEM, PBS_EVENTCLASS_SERVER, meth_name, meth_names[cntr]);
    }
  free(meth_names);
  }


void log_get_set_eventclass(

  int *objclass, 
  SGetter action)

  {
  static int log_objclass = 0;

  if (action == SETV)
    log_objclass = *objclass;
  else if (action == GETV)
    *objclass = log_objclass;

  } /* end of log_get_set_evnetclass */

void log_format_trq_timestamp(

  char *time_formatted_str, 
  unsigned int buflen)

  {
  char           buffer[30];
  struct timeval tv;
  time_t         curtime;
  struct tm      result;
  unsigned int   milisec=0;

  gettimeofday(&tv, NULL); 
  curtime=tv.tv_sec;

  localtime_r(&curtime, &result);
  strftime(buffer,30,"%Y-%m-%d %H:%M:%S.", &result);
  milisec = tv.tv_usec/100;
  snprintf(time_formatted_str, buflen, "%s%04d", buffer, milisec);
  } /* end of log_format_trq_timestamp */

void log_set_hostname_sharelogging(const char *server_name, const char *server_port)
  {
  char ip[64];
  char hostnm[1024];
  char *hostname = NULL;
  struct hostent *he;
  struct in_addr **addr_list;
         
  if (server_name)
     hostname = (char *)server_name;
  else if (gethostname(hostnm, sizeof(hostnm)) == 0)
     hostname = hostnm;

  if (hostname) 
    {
    if ((he = gethostbyname(hostname)) == NULL) 
      {
      strcpy(ip, "null");
      }
    else
      {
      addr_list = (struct in_addr **) he->h_addr_list;
      if (addr_list[0]) 
        snprintf(ip , sizeof(ip), "%s", inet_ntoa(*addr_list[0]));
      else
        strcpy(ip, "null");
      }
    }
  else
    {
    strcpy(ip, "null");
    strcpy(hostnm, "null");
    hostname = hostnm;
    }

  snprintf(log_host_port, sizeof(log_host_port), "%s:%s (host: %s)", 
    ip, server_port, hostname);
  }

void log_get_host_port(char *host_n_port, size_t s)
{
  snprintf(host_n_port, s, "%s", log_host_port);
}
/* END pbs_log.c */
