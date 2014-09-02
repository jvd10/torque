#include "license_pbs.h" /* See here for the software license */
/*
 * pbs_dsh - a distribute task program using the Task Management API
 * Note that under particularly high workloads, the pbsdsh command may not function properly.
 */

#include <pbs_config.h>   /* the master config generated by configure */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <strings.h>
#include <string.h>
#include <signal.h>
#include <sys/signal.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include "tm.h"
#include "mcom.h"
#include "../lib/Libifl/lib_ifl.h" /* DIS_tcp_setup, DIS_tcp_cleanup */

extern int *tm_conn;
extern int event_count;

#ifndef PBS_MAXNODENAME
#define PBS_MAXNODENAME 80
#endif
#define RESCSTRLEN (PBS_MAXNODENAME+200)

/*
 * a bit of code to map a tm_ error number to the symbol
 */

struct tm_errcode
  {
  int trc_code;
  const char   *trc_name;
  } tm_errcode[] =

  {
    { TM_ESYSTEM, "TM_ESYSTEM" },
  { TM_ENOEVENT, "TM_ENOEVENT" },
  { TM_ENOTCONNECTED, "TM_ENOTCONNECTED" },
  { TM_EUNKNOWNCMD, "TM_EUNKNOWNCMD" },
  { TM_ENOTIMPLEMENTED, "TM_ENOTIMPLEMENTED" },
  { TM_EBADENVIRONMENT, "TM_EBADENVIRONMENT" },
  { TM_ENOTFOUND, "TM_ENOTFOUND" },
  { 0,  "?" }
  };

int            *ev;
tm_event_t     *events_spawn;
tm_event_t     *events_obit;
int             numnodes;
tm_task_id     *tid;
int             verbose = 0;
sigset_t        allsigs;
char           *id;
struct tm_roots rootrot;

int stdoutfd, stdoutport;
fd_set permrfsd;
int grabstdio = 0;


const char *get_ecname(

  int rc)

  {

  struct tm_errcode *p;

  for (p = &tm_errcode[0];p->trc_code;++p)
    {
    if (p->trc_code == rc)
      break;
    }

  return(p->trc_name);
  }

int fire_phasers = 0;

void bailout(

  int sig)

  {
  fire_phasers = sig;

  return;
  }



/*
 * obit_submit - submit an OBIT request
 * FIXME: do we need to retry this multiple times?
 */

int obit_submit(

  int c)     /* the task index number */

  {
  int rc;

  if (verbose)
    {
    fprintf(stderr, "%s: sending obit for task %d\n",
            id,
            c);
    }

  rc = tm_obit(*(tid + c), ev + c, events_obit + c);

  if (rc == TM_SUCCESS)
    {
    if (*(events_obit + c) == TM_NULL_EVENT)
      {
      if (verbose)
        {
        fprintf(stderr, "%s: task already dead\n", id);
        }
      }
    else if (*(events_obit + c) == TM_ERROR_EVENT)
      {
      if (verbose)
        {
        fprintf(stderr, "%s: Error on Obit return\n", id);
        }
      }
    }
  else
    {
    fprintf(stderr, "%s: failed to register for task termination notice, task %d\n",
            id,
            c);
    }

  return(rc);
  }  /* END obit_submit() */




/*
 * mom_reconnect - continually attempt to reconnect to mom
 * If we do reconnect, resubmit OBIT requests
 *
 * FIXME: there's an assumption that all tasks have already been
 * spawned and initial OBIT requests have been made.
 */

void
mom_reconnect(void)

  {
  int c, rc;

  for (;;)
    {
    tm_finalize();

    sigprocmask(SIG_UNBLOCK, &allsigs, NULL);

    sleep(2);

    sigprocmask(SIG_BLOCK, &allsigs, NULL);

    /* attempt to reconnect */

    rc = tm_init(0, &rootrot);

    if (rc == TM_SUCCESS)
      {
      fprintf(stderr, "%s: reconnected\n",
              id);

      /* resend obit requests */

      for (c = 0;c < numnodes;++c)
        {
        if (*(events_obit + c) != TM_NULL_EVENT)
          {
          rc = obit_submit(c);

          if (rc != TM_SUCCESS)
            {
            break;  /* reconnect again */
            }
          }
        else if (verbose)
          {
          fprintf(stderr, "%s: skipping obit resend for %u\n",
                  id,
                  *(tid + c));
          }
        }

      break;
      }
    }

  return;
  }  /* END mom_reconnect() */


void
getstdout(void)
  {

  struct timeval tv =
    {
    0, 10000
    };

  fd_set rfsd;
  int newfd, i;
  char buf[1024];
  ssize_t bytes;
  int ret;
  static int maxfd = -1;
  int flags;

  if (maxfd == -1)
    {
    if (stdoutfd > *tm_conn)
      maxfd = stdoutfd;
    else
      maxfd = *tm_conn;
    }

  rfsd = permrfsd;

  if (maxfd < (int)FD_SETSIZE)
    FD_SET(stdoutfd, &rfsd);

  FD_SET(*tm_conn, &permrfsd);

  if ((ret = select(maxfd + 1, &rfsd, NULL, NULL, &tv)) > 0)
    {
    if (FD_ISSET(*tm_conn, &rfsd))
      {
      return;
      }

    if (FD_ISSET(stdoutfd, &rfsd))
      {
      newfd = accept(stdoutfd, NULL, NULL);

      if (newfd > maxfd)
        maxfd = newfd;

      flags = fcntl(newfd, F_GETFL);

#if defined(FNDELAY) && !defined(__hpux)
      flags |= FNDELAY;

#else
      flags |= O_NONBLOCK;

#endif
      fcntl(newfd, F_SETFL, flags);

      FD_SET(newfd, &permrfsd);

      FD_CLR(stdoutfd, &rfsd);

      ret--;
      }

    if (ret)
      {
      for (i = 0; i <= maxfd; i++)
        {
        if (FD_ISSET(i, &rfsd))
          {
          if ((bytes = read_ac_socket(i, &buf, 1023)) > 0)
            {
            buf[bytes] = '\0';
            fprintf(stdout, "%s", buf);
            }
          else if (bytes == 0)
            {
            FD_CLR(i, &permrfsd);
            close(i);

            if (i == maxfd)
              {
              int j;
              maxfd = stdoutfd;

              for (j = 0; j < i; j++)
                if (FD_ISSET(j, &permrfsd))
                  if (j > maxfd)
                    maxfd = j;
              }
            }
          else
            {
            fprintf(stderr, "%s: error in read\n", id);
            }

          ret--;

          if (ret <= 0)
            break;
          }
        }
      }
    }
  }




/*
 * wait_for_task - wait for all spawned tasks to
 * a. have the spawn acknowledged, and
 * b. the task to terminate and return the obit with the exit status
 */

int wait_for_task(

  int *nspawned) /* number of tasks spawned */

  {
  int     c;
  tm_event_t  eventpolled;
  int     nobits = 0;
  int     rc;
  int     tm_errno;

  while (*nspawned || nobits)
    {
    if (grabstdio)
      getstdout();

    if (fire_phasers)
      {
      tm_event_t event;

      for (c = 0;c < numnodes;c++)
        {
        if (*(tid + c) == TM_NULL_TASK)
          continue;

        fprintf(stderr, "%s: killing task %u signal %d\n",
          id,
          *(tid + c),
          fire_phasers);

        tm_kill(*(tid + c), fire_phasers, &event);
        }

      tm_finalize();

      exit(1);
      }

    sigprocmask(SIG_UNBLOCK, &allsigs, NULL);

    rc = tm_poll(TM_NULL_EVENT, &eventpolled, !grabstdio, &tm_errno);

    sigprocmask(SIG_BLOCK, &allsigs, NULL);

    if (rc != TM_SUCCESS)
      {
      fprintf(stderr, "%s: Event poll failed, error %s\n",
        id,
        get_ecname(rc));

      if (rc == TM_ENOTCONNECTED)
        {
        mom_reconnect();
        }
      else
        {
        exit(2);
        }
      }

    if (eventpolled == TM_NULL_EVENT)
      continue;

    for (c = 0;c < numnodes;++c)
      {
      if (eventpolled == *(events_spawn + c))
        {
        /* spawn event returned - register obit */

        if (verbose)
          {
          fprintf(stderr, "%s: spawn event returned: %d (%d spawns and %d obits outstanding)\n",
                  id,
                  c,
                  *nspawned,
                  nobits);
          }

        (*nspawned)--;

        if (tm_errno)
          {
          fprintf(stderr, "%s: error %d on spawn\n",
            id,
            tm_errno);

          continue;
          }

        rc = obit_submit(c);

        if (rc == TM_SUCCESS)
          {
          if ((*(events_obit + c) != TM_NULL_EVENT) &&
              (*(events_obit + c) != TM_ERROR_EVENT))
            {
            nobits++;
            }
          }
        }
      else if (eventpolled == *(events_obit + c))
        {
        /* obit event, let's check it out */

        if (tm_errno == TM_ESYSTEM)
          {
          if (verbose)
            {
            fprintf(stderr, "%s: error TM_ESYSTEM on obit (resubmitting)\n",
              id);
            }

          sleep(2);  /* Give the world a second to take a breath */

          obit_submit(c);

          continue; /* Go poll again */
          }

        if (tm_errno != 0)
          {
          fprintf(stderr, "%s: error %d on obit for task %d\n",
            id,
            tm_errno,
            c);
          }

        /* task exited */

        if (verbose)
          {
          fprintf(stderr, "%s: obit event returned: %d (%d spawns and %d obits outstanding)\n",
                  id,
                  c,
                  *nspawned,
                  nobits);
          }

        nobits--;

        *(tid + c) = TM_NULL_TASK;

        *(events_obit + c) = TM_NULL_EVENT;

        if ((verbose != 0) || (*(ev + c) != 0))
          {
          fprintf(stderr, "%s: task %d exit status %d\n",
                  id,
                  c,
                  *(ev + c));
          }
        }
      }
    }

  return PBSE_NONE;
  }  /* END wait_for_task() */




/* ask TM for all node resc descriptions and parse the output
 * for hostnames */

char *gethostnames(

  tm_node_id *nodelist)

  {
  char *allnodes;
  char *rescinfo;
  tm_event_t *rescevent;
  tm_event_t resultevent;
  char *hoststart;
  int rc, tm_errno, i, j;

  allnodes = (char *)calloc(numnodes, PBS_MAXNODENAME + 1 + sizeof(char));
  rescinfo = (char *)calloc(numnodes, RESCSTRLEN + 1 + sizeof(char));
  rescevent = (tm_event_t*)calloc(numnodes, sizeof(tm_event_t));

  if (!allnodes || !rescinfo || !rescevent)
    {
    fprintf(stderr, "%s: calloc failed!\n",
      id);
    tm_finalize();

    exit(1);
    }

  /* submit resource requests */

  for (i = 0;i < numnodes;i++)
    {
    if (tm_rescinfo(
          nodelist[i],
          rescinfo + (i*RESCSTRLEN),
          RESCSTRLEN - 1,
          rescevent + i) != TM_SUCCESS)
      {
      fprintf(stderr, "%s: error from tm_rescinfo()\n", id);

      tm_finalize();

      exit(1);
      }
    }

  /* read back resource requests */

  for (j = 0, i = 0; i < numnodes; i++)
    {
    rc = tm_poll(TM_NULL_EVENT, &resultevent, 1, &tm_errno);

    if ((rc != TM_SUCCESS) || (tm_errno != TM_SUCCESS))
      {
      fprintf(stderr, "%s: error from tm_poll() %d\n",
        id,
        rc);

      tm_finalize();

      exit(1);
      }

    for (j = 0; j < numnodes; j++)
      {
      if (*(rescevent + j) == resultevent)
        break;
      }

    if (j == numnodes)
      {
      fprintf(stderr, "%s: unknown resource result\n", id);
      tm_finalize();
      exit(1);
      }

    if (verbose)
      fprintf(stderr, "%s: rescinfo from %d: %s\n", id, j, rescinfo + (j*RESCSTRLEN));

    strtok(rescinfo + (j*RESCSTRLEN), " ");

    hoststart = strtok(NULL, " ");

    if (hoststart == NULL)
      {
      fprintf(stderr, "%s: can't find a hostname in resource result\n", id);
      tm_finalize();
      exit(1);
      }

    strcpy(allnodes + (j*PBS_MAXNODENAME), hoststart);
    }

  free(rescinfo);

  free(rescevent);

  return(allnodes);
  }

/* return a vnode number matching targethost */
int findtargethost(char *allnodes, char *targethost)
  {
  int i;
  char *ptr;
  int vnode = 0;

  if ((ptr = strchr(targethost, '/')) != NULL)
    {
    *ptr = '\0';
    ptr++;
    vnode = atoi(ptr);
    }

  for (i = 0; i < numnodes; i++)
    {
    if (!strcmp(allnodes + (i*PBS_MAXNODENAME), targethost))
      {
      if (vnode == 0)
        return(i);

      vnode--;
      }
    else
      {
      /* Sometimes the allnodes will return the FQDN of the host
       and the PBS_NODEFILE will have the short name of the host.
       See if the shortname mataches */
      std::string targetname(targethost);
      std::string the_host(allnodes + (i*PBS_MAXNODENAME));
      std::size_t dot = the_host.find_first_of(".");
      if(dot != std::string::npos)
        {
        the_host[dot] = '\0';
        std::string shortname(the_host.c_str());
        if (shortname.compare(targetname) == 0)
          {
          if (vnode == 0)
            return(i);
          vnode--;
          }
        }
      } 
    }

  if (i == numnodes)
    {
    fprintf(stderr, "%s: %s not found\n", id, targethost);
    tm_finalize();
    exit(1);
    }

  return(-1);
  }

/* prune nodelist down to a unique list by comparing with
 * the hostnames in all nodes */
int uniquehostlist(tm_node_id *nodelist, char *allnodes)
  {
  int hole, i, j, umove = 0;

  for (hole = numnodes, i = 0, j = 1; j < numnodes; i++, j++)
    {
    if (strcmp(allnodes + (i*PBS_MAXNODENAME), allnodes + (j*PBS_MAXNODENAME)) == 0)
      {
      if (!umove)
        {
        umove = 1;
        hole = j;
        }
      }
    else if (umove)
      {
      nodelist[hole++] = nodelist[j];
      }
    }

  return(hole);
  }

static int build_listener(

  int *port)

  {
  int s;

  struct sockaddr_in addr;
  torque_socklen_t len = sizeof(addr);

  if ((s = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    fprintf(stderr, "%s: socket", id);
  else
    {
    if (listen(s, 1024) < 0)
      fprintf(stderr, "%s: listen", id);
    else
      {
      if (getsockname(s, (struct sockaddr *)&addr, &len) < 0)
        {
        fprintf(stderr, "%s: getsockname", 
          id);
        }
      else
        *port = ntohs(addr.sin_port);
      }
    }


  return (s);
  }




int main(

  int   argc,
  char *argv[])

  {
  int c;
  int err = 0;
  int ncopies = -1;
  int onenode = -1;
  int rc;

  int  nspawned = 0;
  tm_node_id *nodelist = NULL;
  int start;
  int stop;
  int sync = 0;

  int pernode = 0;
  char *targethost = NULL;
  char *allnodes;

  struct sigaction act;

  char **ioenv;

  extern int   optind;
  extern char *optarg;

  int posixly_correct_set_by_caller = 0;
  char *envstr;

  id = (char *)calloc(60, sizeof(char));

  if (id == NULL)
    {
    fprintf(stderr, "%s: calloc failed, (%d)\n",
      id,
      errno);

    return(1);
    }

  sprintf(id, "pbsdsh(%s)",
          ((getenv("PBSDEBUG") != NULL) && (getenv("PBS_TASKNUM") != NULL))
          ? getenv("PBS_TASKNUM")
          : "");

#ifdef __GNUC__
  /* If it's already set, we won't unset it later */

  if (getenv("POSIXLY_CORRECT") != NULL)
    posixly_correct_set_by_caller = 1;

  envstr = strdup("POSIXLY_CORRECT=1");

  putenv(envstr);

#endif

  while ((c = getopt(argc, argv, "c:n:h:osuv")) != EOF)
    {
    switch (c)
      {

      case 'c':

        ncopies = atoi(optarg);

        if (ncopies <= 0)
          {
          err = 1;
          }

        break;

      case 'h':

        targethost = strdup(optarg); /* run on this 1 hostname */

        break;

      case 'n':

        onenode = atoi(optarg);

        if (onenode < 0)
          {
          err = 1;
          }

        break;

      case 'o':

        grabstdio = 1;

        break;

      case 's':

        sync = 1; /* force synchronous spawns */

        break;

      case 'u':

        pernode = 1; /* run once per node (unique hostnames) */

        break;

      case 'v':

        verbose = 1; /* turn on verbose output */

        break;

      default:

        err = 1;

        break;
      }  /* END switch (c) */

    }    /* END while ((c = getopt()) != EOF) */

  if ((err != 0) || ((onenode >= 0) && (ncopies >= 1)))
    {
    fprintf(stderr, "Usage: %s [-c copies][-o][-s][-u][-v] program [args]...]\n",
      argv[0]);

    fprintf(stderr, "       %s [-n nodenumber][-o][-s][-u][-v] program [args]...\n",
      argv[0]);

    fprintf(stderr, "       %s [-h hostname][-o][-v] program [args]...\n",
      argv[0]);

    fprintf(stderr, "Where -c copies =  run  copy of \"args\" on the first \"copies\" nodes,\n");
    fprintf(stderr, "      -n nodenumber = run a copy of \"args\" on the \"nodenumber\"-th node,\n");
    fprintf(stderr, "      -o = capture stdout of processes,\n");
    fprintf(stderr, "      -s = forces synchronous execution,\n");
    fprintf(stderr, "      -u = run on unique hostnames,\n");
    fprintf(stderr, "      -h = run on this specific hostname,\n");
    fprintf(stderr, "      -v = forces verbose output.\n");

    exit(1);
    }

#ifdef __GNUC__
  if (!posixly_correct_set_by_caller)
    {
    putenv((char *)"POSIXLY_CORRECT");
    free(envstr);
    }

#endif


  if (getenv("PBS_ENVIRONMENT") == NULL)
    {
    fprintf(stderr, "%s: not executing under PBS\n",
      id);

    return(1);
    }

  if (grabstdio && sync)
    {
    fprintf(stderr, "Warning: options -s and -o should not be used together;\n");
    fprintf(stderr, "  otherwise, only the output from the 1st host will be shown.\n\n");
    }


  /*
   * Set up interface to the Task Manager
   */

  if ((rc = tm_init(0, &rootrot)) != TM_SUCCESS)
    {
    fprintf(stderr, "%s: tm_init failed, rc = %s (%d)\n",
      id,
      get_ecname(rc),
      rc);

    return(1);
    }

  sigemptyset(&allsigs);

  sigaddset(&allsigs, SIGHUP);
  sigaddset(&allsigs, SIGINT);
  sigaddset(&allsigs, SIGTERM);

  act.sa_mask = allsigs;
  act.sa_flags = 0;

  /* We want to abort system calls and call a function. */

#ifdef SA_INTERRUPT
  act.sa_flags |= SA_INTERRUPT;
#endif
  act.sa_handler = bailout;
  sigaction(SIGHUP, &act, NULL);
  sigaction(SIGINT, &act, NULL);
  sigaction(SIGTERM, &act, NULL);

#ifdef DEBUG

  if (rootrot.tm_parent == TM_NULL_TASK)
    {
    fprintf(stderr, "%s: I am the mother of all tasks\n",
      id);
    }
  else
    {
    fprintf(stderr, "%s: I am but a child in the scheme of things\n",
      id);
    }

#endif /* DEBUG */

  if ((rc = tm_nodeinfo(&nodelist, &numnodes)) != TM_SUCCESS)
    {
   fprintf(stderr, "%s: tm_nodeinfo failed, rc = %s (%d) nodelist= %d numnodes= %d\n",
      id,
      get_ecname(rc),
      rc,
      (nodelist==NULL) ? -1 : *nodelist,
      numnodes);

    return(1);
    }

  /* nifty unique/hostname code */
  if (pernode || targethost)
    {
    allnodes = gethostnames(nodelist);

    if (targethost)
      {
      onenode = findtargethost(allnodes, targethost);
      }
    else
      {
      numnodes = uniquehostlist(nodelist, allnodes);
      }

    free(allnodes);

    if (targethost)
      free(targethost);
    }

  /* We already checked the lower bounds in the argument processing,
     now we check the upper bounds */

  if ((onenode >= numnodes) || (ncopies > numnodes))
    {
    fprintf(stderr, "%s: only %d nodes available\n",
      id,
      numnodes);

    return(1);
    }

  /* calloc space for various arrays based on number of nodes/tasks */

  tid = (tm_task_id *)calloc(numnodes, sizeof(tm_task_id));

  events_spawn = (tm_event_t *)calloc(numnodes, sizeof(tm_event_t));

  events_obit  = (tm_event_t *)calloc(numnodes, sizeof(tm_event_t));

  ev = (int *)calloc(numnodes, sizeof(int));

  if ((tid == NULL) ||
      (events_spawn == NULL) ||
      (events_obit == NULL) ||
      (ev == NULL))
    {
    /* FAILURE - cannot alloc memory */

    fprintf(stderr, "%s: memory alloc of task ids failed\n",
      id);

    return(1);
    }

  for (c = 0;c < numnodes;c++)
    {
    *(tid + c)          = TM_NULL_TASK;
    *(events_spawn + c) = TM_NULL_EVENT;
    *(events_obit  + c) = TM_NULL_EVENT;
    *(ev + c)           = 0;
    }  /* END for (c) */

  /* Now spawn the program to where it goes */

  if (onenode >= 0)
    {
    /* Spawning one copy onto logical node "onenode" */

    start = onenode;
    stop  = onenode + 1;
    }
  else if (ncopies >= 0)
    {
    /* Spawn a copy of the program to the first "ncopies" nodes */

    start = 0;
    stop  = ncopies;
    }
  else
    {
    /* Spawn a copy on all nodes */

    start = 0;
    stop  = numnodes;
    }

  if ((ioenv = (char **)calloc(2, sizeof(char **))) == NULL)
    {
    /* FAILURE - cannot alloc memory */

    fprintf(stderr,"%s: memory alloc of ioenv failed\n",
      id);

    return(1);
    }

  if (grabstdio != 0)
    {
    stdoutfd = build_listener(&stdoutport);

    if ((*ioenv = (char *)calloc(50, sizeof(char))) == NULL)
      {
      /* FAILURE - cannot alloc memory */

      fprintf(stderr,"%s: memory alloc of *ioenv failed\n",
        id);

      return(1);
      }

    snprintf(*ioenv,49,"TM_STDOUT_PORT=%d", 
      stdoutport);

    FD_ZERO(&permrfsd);
    }

  sigprocmask(SIG_BLOCK, &allsigs, NULL);

  for (c = start; c < stop; ++c)
    {
    if ((rc = tm_spawn(
                argc - optind,
                argv + optind,
                ioenv,
                *(nodelist + c),
                tid + c,
                events_spawn + c)) != TM_SUCCESS)
      {
      fprintf(stderr, "%s: spawn failed on node %d err %s\n",
        id,
        c,
        get_ecname(rc));
      }
    else
      {
      if (verbose)
        fprintf(stderr, "%s: spawned task %d\n",
          id,
          c);

      ++nspawned;

      if (sync)
        rc = wait_for_task(&nspawned); /* one at a time */
      }
    }    /* END for (c) */

  if (sync == 0)
    rc = wait_for_task(&nspawned); /* wait for all to finish */
  if (rc != 0)
    return rc;

  /*
   * Terminate interface with Task Manager
   */

  tm_finalize();

  return 0;
  }  /* END main() */

/* END pbsdsh.c */
