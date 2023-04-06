/*
 *
 * This program implements a tiny shell with job control.
 *
 */

#include <sys/types.h>
#include <sys/wait.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// You may assume that these constants are large enough.
#define MAXLINE      1024   // max line size
#define MAXARGS       128   // max args on a command line
#define MAXJOBS        16   // max jobs at any point in time
#define MAXJID   (1 << 16)  // max job ID

// The job states are:
#define UNDEF 0 // undefined
#define FG 1    // running in foreground
#define BG 2    // running in background
#define ST 3    // stopped

/*
 * The job state transitions and enabling actions are:
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : fg command
 *     ST -> BG  : bg command
 *     BG -> FG  : fg command
 * At most one job can be in the FG state.
 */

struct Job {
	pid_t pid;              // job PID
	int jid;                // job ID [1, 2, ...]
	int state;              // UNDEF, FG, BG, or ST
	char cmdline[MAXLINE];  // command line
};
typedef volatile struct Job *JobP;

/*
 * Define the jobs list using the "volatile" qualifier because it is accessed
 * by a signal handler (as well as the main program).
 */
static volatile struct Job jobs[MAXJOBS];
static int nextjid = 1;            // next job ID to allocate

extern char **environ;             // defined by libc

static char prompt[] = "tsh> ";    // command line prompt (DO NOT CHANGE)
static bool verbose = false;       // If true, print additional output.

/*
 * The following array can be used to map a signal number to its name.
 * This mapping is valid for x86(-64)/Linux systems, such as CLEAR.
 * The mapping for other versions of Unix, such as FreeBSD, Mac OS X, or
 * Solaris, differ!
 */
static const char *const signame[NSIG] = {
	"Signal 0",
	"HUP",				/* SIGHUP */
	"INT",				/* SIGINT */
	"QUIT",				/* SIGQUIT */
	"ILL",				/* SIGILL */
	"TRAP",				/* SIGTRAP */
	"ABRT",				/* SIGABRT */
	"BUS",				/* SIGBUS */
	"FPE",				/* SIGFPE */
	"KILL",				/* SIGKILL */
	"USR1",				/* SIGUSR1 */
	"SEGV",				/* SIGSEGV */
	"USR2",				/* SIGUSR2 */
	"PIPE",				/* SIGPIPE */
	"ALRM",				/* SIGALRM */
	"TERM",				/* SIGTERM */
	"STKFLT",			/* SIGSTKFLT */
	"CHLD",				/* SIGCHLD */
	"CONT",				/* SIGCONT */
	"STOP",				/* SIGSTOP */
	"TSTP",				/* SIGTSTP */
	"TTIN",				/* SIGTTIN */
	"TTOU",				/* SIGTTOU */
	"URG",				/* SIGURG */
	"XCPU",				/* SIGXCPU */
	"XFSZ",				/* SIGXFSZ */
	"VTALRM",			/* SIGVTALRM */
	"PROF",				/* SIGPROF */
	"WINCH",			/* SIGWINCH */
	"IO",				/* SIGIO */
	"PWR",				/* SIGPWR */
	"Signal 31"
};

// You must implement the following functions:

static bool	builtin_cmd(char **argv);
static void	do_bgfg(char **argv); 
static void	eval(const char *cmdline);
static void	initpath(const char *pathstr);
static void	waitfg(pid_t pid);

static void	sigchld_handler(int signum);
static void	sigint_handler(int signum);
static void	sigtstp_handler(int signum);

// We are providing the following functions to you:

static bool	parseline(const char *cmdline, char **argv);

static void	sigquit_handler(int signum);

static bool	addjob(JobP jobs, pid_t pid, int state, const char *cmdline);
static void	clearjob(JobP job);
static bool	deletejob(JobP jobs, pid_t pid);
static pid_t	fgpid(JobP jobs);
static JobP	getjobjid(JobP jobs, int jid); 
static JobP	getjobpid(JobP jobs, pid_t pid);
static void	initjobs(JobP jobs);
static void	listjobs(JobP jobs);
static int	maxjid(JobP jobs); 
static int	pid2jid(pid_t pid); 

static void	app_error(const char *msg);
static void	unix_error(const char *msg);
static void	usage(void);

static void	Sio_error(const char s[]);
static ssize_t	Sio_putl(long v);
static ssize_t	Sio_puts(const char s[]);
static void	sio_error(const char s[]);
static void	sio_ltoa(long v, char s[], int b);
static ssize_t	sio_putl(long v);
static ssize_t	sio_puts(const char s[]);
static void	sio_reverse(char s[]);
static size_t	sio_strlen(const char s[]);


/* Global variable that holds the search path*/
static char **path_values;

/*
 * Requires:
 *   argc is the number of arguments in argv.
 *   argv is an array of strings.
 *
 * Effects:
 *   Runs the main program that executes shell.
 *   It parses through the command line, initializing the handlers,
 *   the search path, the jobs list, and executing the shell's read/eval loop.
 */
int
main(int argc, char **argv) 
{
	struct sigaction action;
	int c;
	char cmdline[MAXLINE];
	char *path = NULL;
	bool emit_prompt = true;	// Emit a prompt by default.

	/*
	 * Redirect stderr to stdout (so that driver will get all output
	 * on the pipe connected to stdout).
	 */
	if (dup2(1, 2) < 0)
		unix_error("dup2 error");

	// Parse the command line.
	while ((c = getopt(argc, argv, "hvp")) != -1) {
		switch (c) {
		case 'h':             // Print a help message.
			usage();
			break;
		case 'v':             // Emit additional diagnostic info.
			verbose = true;
			break;
		case 'p':             // Don't print a prompt.
			// This is handy for automatic testing.
			emit_prompt = false;
			break;
		default:
			usage();
		}
	}

	/*
	 * Install sigint_handler() as the handler for SIGINT (ctrl-c).  SET
	 * action.sa_mask TO REFLECT THE SYNCHRONIZATION REQUIRED BY YOUR
	 * IMPLEMENTATION OF sigint_handler().
	 */
	action.sa_handler = sigint_handler;
	action.sa_flags = SA_RESTART;
	if (sigemptyset(&action.sa_mask) < 0)
		unix_error("sigemptyset error");

	if (sigaction(SIGINT, &action, NULL) < 0)
		unix_error("sigaction error");

	if (sigaddset(&action.sa_mask, SIGCHLD) < 0 )
	 	unix_error("sigaddset error");

	if (sigaddset(&action.sa_mask, SIGTSTP) < 0 )
		unix_error("sigaddset error");	


	/*
	 * Install sigtstp_handler() as the handler for SIGTSTP (ctrl-z).  SET
	 * action.sa_mask TO REFLECT THE SYNCHRONIZATION REQUIRED BY YOUR
	 * IMPLEMENTATION OF sigtstp_handler().
	 */

	action.sa_handler = sigtstp_handler;
	action.sa_flags = SA_RESTART;
	if (sigemptyset(&action.sa_mask) < 0)
		unix_error("sigemptyset error");

	if (sigaction(SIGTSTP, &action, NULL) < 0)
		unix_error("sigaction error");

	 if (sigaddset(&action.sa_mask, SIGCHLD) < 0 )
	 	unix_error("sigaddset error");

	 if (sigaddset(&action.sa_mask, SIGINT) < 0)
		unix_error("sigaddset error");	

	

	/*
	 * Install sigchld_handler() as the handler for SIGCHLD (terminated or
	 * stopped child).  SET action.sa_mask TO REFLECT THE SYNCHRONIZATION
	 * REQUIRED BY YOUR IMPLEMENTATION OF sigchld_handler().
	 */
	action.sa_handler = sigchld_handler;
	action.sa_flags = SA_RESTART;
	if (sigemptyset(&action.sa_mask) < 0)
		unix_error("sigemptyset error");

	if (sigaction(SIGCHLD, &action, NULL) < 0)
		unix_error("sigaction error");

	  if (sigaddset(&action.sa_mask, SIGTSTP) < 0 )
	 	unix_error("sigaddset error");

	 if (sigaddset(&action.sa_mask, SIGINT) < 0)
		unix_error("sigaddset error");

	/*
	 * Install sigquit_handler() as the handler for SIGQUIT.  This handler
	 * provides a clean way for the test harness to terminate the shell.
	 * Preemption of the processor by the other signal handlers during
	 * sigquit_handler() does no harm, so action.sa_mask is set to empty.
	 */
	action.sa_handler = sigquit_handler;
	action.sa_flags = SA_RESTART;
	if (sigemptyset(&action.sa_mask) < 0)
		unix_error("sigemptyset error");

	if (sigaction(SIGQUIT, &action, NULL) < 0)
		unix_error("sigaction error");

	// Initialize the search path.
	path = getenv("PATH"); 	
	initpath(path);

	// Initialize the jobs list.
	initjobs(jobs);

	// Execute the shell's read/eval loop.
	while (true) {

		// Read the command line.
		if (emit_prompt) {
			printf("%s", prompt);
			fflush(stdout);
		}

		if (fgets(cmdline, MAXLINE, stdin) == NULL && ferror(stdin))
			app_error("fgets error");
		
		if (feof(stdin)) // End of file (ctrl-d)
			exit(0);

		// Evaluate the command line.
		eval(cmdline);
		fflush(stdout);
	}

	// Control never reaches here.
	assert(false);
}

/* 
 * eval - Evaluate the command line that the user has just typed in.
 * 
 * If the user has requested a built-in command (quit, jobs, bg or fg)
 * then execute it immediately.  Otherwise, fork a child process and
 * run the job in the context of the child.  If the job is running in
 * the foreground, wait for it to terminate and then return.  Note:
 * each child process must have a unique process group ID so that our
 * background children don't receive SIGINT (SIGTSTP) from the kernel
 * when we type ctrl-c (ctrl-z) at the keyboard.  
 *
 * Requires:
 *   "cmdline" is a NUL ('\0') terminated string with a trailing
 *   '\n' character.  "cmdline" must contain less than MAXARGS arguments.
 *
 * Effects:
 *   Evaluates the command line and executes the command as necessary if exists.
 *   prints and error if the command does not found.
 */
static void
eval(const char *cmdline) 
{
	char *argv[MAXARGS]; // Argument list execve();
	char buf[MAXLINE];   // Holds modified command line.
	bool bg; 
	pid_t pid; 	     // Process id.

	sigset_t mask; 		
	
	/* Step 1: call parsline to create <agrv> */
	strcpy(buf, cmdline); 		// Copies the string in cmdline to buf. 
	bg = parseline(buf, argv);  // Parseline builds argv**.
	if (argv[0] == NULL)
		return; 	   			// Ignore empty lines.

	/* Step 2: call builtin_cmd to determine if first word is built-in command*/
	if (!builtin_cmd(argv)) { 
		if (sigemptyset(&mask) < 0) 
			unix_error("sigemptyset error");
		
		if (sigaddset(&mask, SIGCHLD) < 0 ) 
			unix_error("sigaddset error");
		
		if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0 )
			unix_error("sigprocmask error");

		/*
		 *  ------ HERE: THE FIRST WORD IS NOT BUILD IN COMMAND -------
		 * The first word in the command line is the name of a program to execute.
		 * In this case, the shell forks a child process, then attempts to load and run the program
		 * in the context of the child. 
		 *
		 * The child processes created as a result of interpreting a single command line
		 * are known collectively as a job.
		 * In general, a job can consist of multiple child processes connected by Unix pipes.
		 */

		pid = fork();  		
		if (pid == 0) {

			setpgid(0, 0);
			sigprocmask(SIG_UNBLOCK, &mask, NULL); 

	
			/* 
			 * Attempts to load and run the program in the context of the child.
			 * If the first word in the command line starts with a directory,
			 * such as “.”, “/”, or the name of a subdirectory in the current directory,
			 * the word is assumed to be the path of an executable program.
			 * 
			 * Otherwise, the first word is assumed to be the name of an executable 
			 * that is contained in one of the directories in the shell’s search path (PATH_VALUES),
			 * which is an ordered list of directories that the shell searches for executables.
			 */ 

			//Attempt: the first word in the command line contains "/", so run the path and the executable.
			if (strstr(argv[0], "/") != NULL) {
				// The word is assumed to be the path name of an executable program. 
				if (execve(argv[0], argv, environ) < 0) {
					printf("%s: Command not found.\n", argv[0]);
					exit(0); // Exits child.
				}
			}


			/*
			 * Attempt:
			 * The first word is assumed to be the name of an executable
			 * that is contained in one of the directories in the shell’s search path,
			 * which is an ordered list of directories that the shell searches for executables.
			 */
			int i = 0;
			char *command;
			char *slash = {"/"}; 
			char *p = path_values[i];

			while (p != NULL) {
				command = malloc(strlen(path_values[i]) + strlen(argv[0]) + sizeof(char*));
				strcpy(command, "");		// Reseting the command to an emplty list.
				strcpy(command, p);			// Copying path_value[i] to the command.
				strcat(command, slash);		// Adding "/" to the command.
				strcat(command, argv[0]);	// Adding the first word from the command line to the command.

				if (execve(command, argv, environ) < 0) {
					i++;
					p = path_values[i];
				}
			}
			printf("%s: Command not found.\n", argv[0]);
			exit(0); // Exits child.
		}

		/*Parent wait for bg job to terminate.*/
		if (!bg) { 
			// Not requested to run in bg --> program waits for child to terminate, then reap it.			
			addjob(jobs, pid, FG, cmdline);
			if (sigprocmask(SIG_UNBLOCK, &mask, NULL)) {
				unix_error("sigprocmask error");
			}
			waitfg(pid);

		} else {
			// Requested to run in bg --> print prompt without waiting to terminate.
			addjob(jobs, pid, BG, cmdline);	
			if (sigprocmask(SIG_UNBLOCK, &mask, NULL)) {
				unix_error("sigprocmask error");
			} 
			printf("[%d] (%d) %s", pid2jid(pid), pid, cmdline);
		}
	}
	return;
}

/* 
 * parseline - Parse the command line and build the argv array.
 *
 * Requires:
 *   "cmdline" is a NUL ('\0') terminated string with a trailing
 *   '\n' character.  "cmdline" must contain less than MAXARGS
 *   arguments.
 *
 * Effects:
 *   Builds "argv" array from space delimited arguments on the command line.
 *   The final element of "argv" is set to NULL.  Characters enclosed in
 *   single quotes are treated as a single argument.  Returns true if
 *   the user has requested a BG job and false if the user has requested
 *   a FG job.
 *
 * Note:
 *   In the textbook, this function has the return type "int", but "bool"
 *   is more appropriate.
 */
static bool
parseline(const char *cmdline, char **argv) 
{
	int argc;                   // Number of args.
	static char array[MAXLINE]; // Local copy of command line.
	char *buf = array;          // Ptr that traverses command line.
	char *delim;                // Points to first space delimiter.
	bool bg;                    // Background job.

	strcpy(buf, cmdline);

	// Replace trailing '\n' with space.
	buf[strlen(buf) - 1] = ' ';

	// Ignore leading spaces.
	while (*buf != '\0' && *buf == ' ')
		buf++;

	// Build the argv list.
	argc = 0;
	if (*buf == '\'') {
		buf++;
		delim = strchr(buf, '\'');
	} else
		delim = strchr(buf, ' ');
	while (delim != NULL) {
		argv[argc++] = buf;
		*delim = '\0';
		buf = delim + 1;
		while (*buf != '\0' && *buf == ' ')	// Ignore spaces.
			buf++;
		if (*buf == '\'') {
			buf++;
			delim = strchr(buf, '\'');
		} else
			delim = strchr(buf, ' ');
	}
	argv[argc] = NULL;

	// Ignore blank line.
	if (argc == 0)
		return (true);

	// Should the job run in the background.
	if ((bg = (*argv[argc - 1] == '&')) != 0)
		argv[--argc] = NULL;

	return (bg);
}

/* 
 * builtin_cmd - If the user has typed a built-in command then execute
 *  it immediately. Else - return False.  
 *
 * Requires:
 *   An array of string arguments argv.
 *
 * Effects:
 *   Executes the built in command passed by the argv arguments. 
 *   If not a built in command returns false.
 *
 * Note:
 *   In the textbook, this function has the return type "int", but "bool"
 *   is more appropriate.
 */
static bool
builtin_cmd(char **argv) 
{
	assert(argv[0] != NULL);

	if (!strcmp(argv[0], "quit")) {
		// Quit command.
		exit(0);
	}

	if (!strcmp(argv[0], "jobs")) { 
		// List the stopped jobs and running background jobs. 
		listjobs(jobs);
		return(true);
	}

	if (!strcmp(argv[0], "bg")) {
		// Bg <job>: Change a stopped job to a running background job.
		do_bgfg(argv);
		return(true);
	}

	if (!strcmp(argv[0], "fg")) {
		// Fg <job>: Change a stopped or running background job to a running job in the foreground.
		do_bgfg(argv);
		return(true);
	}

	return(false); // Not a built-in command.
}

/* 
 * do_bgfg - Execute the built-in bg and fg commands.
 *
 * Requires:
 *   An array of string arguments argv containing a PID or JID.
 *
 * Effects:
 *   Depending on the specified input changes a jobs or 
 *   proceses state to background or foreground. 
 *   If input is invalid, writes an error message and returns.
 */
static void
do_bgfg(char **argv) 
{
	if (argv[1] == '\0') {
		printf("%s command requires PID or %%jobid argument\n", argv[0]);
		return;
	}

	JobP currjob = NULL;

	// Check arguments and get the PID or JID.

	// Case: JID.
	if (argv[1][0] == '%') { // If starts with a % then its JID.
		if (!isdigit(argv[1][1])){
			printf("%s: argument must be a PID or %%jobid\n", argv[0]);
		return;
		}	
		currjob = getjobjid(jobs, atoi(&argv[1][1]));
		if (currjob == NULL) {
			printf("%s: No such job\n", argv[1]);
			return;
		}
	} else { // Case: PID.
		if (atoi(argv[1])) { // If starts with a digit then is PID.	
			currjob = getjobpid(jobs, atoi(argv[1]));  
			if (currjob == NULL) {
				printf("(%s): No such process\n", argv[1]);
				return;
			}
		} else {
			
			printf("%s: argument must be a PID or %%jobid\n", argv[0]);
			return;
		}
	}

	
	// Check if its stopped.
	if (currjob->state == ST) {
		// Restart <job> by sending it a SIGCONT signal.
		if (kill(-currjob->pid, SIGCONT) < 0) {
			unix_error("kill call failed");
		}
	}

	// Check if we need bg or fg. 

	// Case: FG.
	if (strcmp(argv[0], "fg") == 0) { 	
			currjob->state = FG; // Change state to FG.
			waitfg(currjob->pid); // Wait for it to run in foreground.
		
	}

	// Case: BG.
	if (strcmp(argv[0], "bg") == 0) {	
		if (currjob != NULL) { 
			currjob->state = BG; // Change state to BG.
			printf("[%d] (%d) %s", pid2jid(currjob->pid), currjob->pid, currjob->cmdline);
		}
	}
	return;
}

/* 
 * waitfg - Block until process pid is no longer the foreground process.
 *
 * Requires:
 *  A valid pid.
 *
 * Effects:
 *   Waits and blocks until the pid is no longer the foreground process.
 */
static void
waitfg(pid_t pid)
{
	
	sigset_t mask;	
	
	if (sigemptyset(&mask) < 0)
		unix_error("sigemptyset error"); 

	while (1) { // While pid is the foreground process continue. 
		if (fgpid(jobs) != pid) {
			return;
		}
		sigsuspend(&mask);
	}
	return;
}

/* 
 * initpath - Perform all necessary initialization of the search path,
 *  which may be simply saving the path.
 *
 * Requires:
 *   "pathstr" is a valid search path.
 *
 * Effects:
 *   Splits the pathsrt based on ":" and stores each substring
 *   in a global array path_values.
 */
static void
initpath(const char *pathstr)
{
	/*
	 * Initpath() gets the environmental variable string.
	 * This is a long string, separated by colons, between each colon is a path.
	 * It then split them into each complete directory path,
	 * and then store in path_values, a global array are strings in our shell program.
	 */

	int count = 0;
	int j = 0;
	char *search = {":"};

	while (pathstr[j] != '\0') {
        // If ':' is found in string then increment count variable.
        if (pathstr[j] == search[0]) {
        	count++;
		}
        j++;
    }

	path_values = malloc((count + 1) * sizeof(char*) ); 
   	int i = 0;
	char *str = malloc(sizeof(pathstr)); 
	str = strcpy(str, pathstr);
   	char *token = strtok(str, ":");
   	while (token != NULL) {
		path_values[i] = malloc(sizeof(token));
		path_values[i] = token;
        token = strtok(NULL, ":");
        i++;
	} 
}

/*
 * The signal handlers follow.
 */

/* 
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *  a child job terminates (becomes a zombie), or stops because it
 *  received a SIGSTOP or SIGTSTP signal.  The handler reaps all
 *  available zombie children, but doesn't wait for any other
 *  currently running children to terminate.  
 *
 * Requires:
 *   A signal sent out.
 *
 * Effects:
 * 
 *   If received a SIGINT signal, prints out a message containing 
 *   the processes information and desciption of termination due to SIGINT 
 *   and deletes the job.
 *   If reveiced a signal of the process being stopped, updates the jobs state to stopped 
 *   and  prints a message with the job’s PID and a description of the offending signal.
 *   Deletes normally terminated jobs.
 * 
 * 
 *   
 */
static void
sigchld_handler(int signum)
{
	pid_t pid; 
	int status;


	//Reap all the zombie processes in a while loop with waitpid.
	while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0) {

		// Get the job associated with pid.
		volatile struct Job *currjob = getjobpid(jobs, pid); 

		// Check the exit status of each child in the while loop .

		// WIFEXITED(status) - true if child terminated normally via call to exit or return. 
		if (WIFEXITED(status)) { 
			// then just delete the job
			deletejob(jobs,pid);
		}

		// WIFSIGNALED(status) - true if terminated becasue of a signal that was not caught.
		if (WIFSIGNALED(status)) { 

			// Then tsh recognizes this event and prints a message with the job’s PID and a description of the offending signal SIGINT.
				
			sio_puts("Job ["); 
			sio_putl(pid2jid(pid)); 
			sio_puts("] ("); 
			sio_putl(pid);
			sio_puts(") terminated by signal SIGINT");
			sio_puts("\n");

			deletejob(jobs, pid);
		} 

		// WIFSTOPPED(status) - true if child is currenlty stopped. 
		if (WIFSTOPPED(status)) {
			currjob->state = ST; 

			// Then tsh recognizes this event and print a message with the job’s PID and a description of the offending signal.
			
			sio_puts("Job [");
			sio_putl(pid2jid(pid)); 
			sio_puts("] ("); 
			sio_putl(pid);
			sio_puts(") stopped by signal SIG");
			sio_puts(signame[WSTOPSIG(status)]); 
			sio_puts("\n");
		}		
	}

	// Prevent an "unused parameter" warning.
	(void)signum;

	return;
}
	

/* 
 * sigint_handler - The kernel sends a SIGINT to the shell whenever the
 *  user types ctrl-c at the keyboard.  Catch it and send it along
 *  to the foreground job.  
 *
 * Requires:
 *   "signum" is SIGINT.
 *
 * Effects:
 *   Sends SIGINT signal to all the processes in the current foreground job group.
 *   If there is no foreground job, no signal is sent.
 */
static void
sigint_handler(int signum)
{
	pid_t pid;

	// Prevent an "unused parameter" warning.
	(void)signum;

	// If there is no foreground job, then the signal should have no effect.

	// Check if there is a foreground job, if true, send SIGINT signal if not dont do anything.
	if ((pid = fgpid(jobs))) {
		kill(-pid, SIGINT); // send out SIGINT to foreground.
	}
	return;	
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *  the user types ctrl-z at the keyboard.  Catch it and suspend the
 *  foreground job by sending it a SIGTSTP.  
 *
 * Requires:
 *   "signum" is SIGTSTP.
 *
 * Effects:
 *   Sends SIGTSTP signal to all processes in the current foreground job group.
 *   If there is no foreground job, no signal is sent.
 */
static void
sigtstp_handler(int signum)
{
	// Prevent an "unused parameter" warning.
	(void)signum;

	pid_t pid;
	// Get pid of the forerground job and check if its equal to 0.
	if ((pid = fgpid(jobs))) {
		kill(-pid, SIGTSTP); // If there is a foreground job, send out SIGTSTP.
	}
	return;	
}

/*
 * sigquit_handler - The driver program can gracefully terminate the
 *  child shell by sending it a SIGQUIT signal.
 *
 * Requires:
 *   "signum" is SIGQUIT.
 *
 * Effects:
 *   Terminates the program.
 */
static void
sigquit_handler(int signum)
{
	// Prevent an "unused parameter" warning.
	(void)signum;
	Sio_puts("Terminating after receipt of SIGQUIT signal\n");
	_exit(1);
}

/*
 * This comment marks the end of the signal handlers.
 */

/*
 * The following helper routines manipulate the jobs list.
 */

/*
 * Requires:
 *   "job" points to a job structure.
 *
 * Effects:
 *   Clears the fields in the referenced job structure.
 */
static void
clearjob(JobP job)
{
	job->pid = 0;
	job->jid = 0;
	job->state = UNDEF;
	job->cmdline[0] = '\0';
}

/*
 * Requires:
 *   "jobs" points to an array of MAXJOBS job structures.
 *
 * Effects:
 *   Initializes the jobs list to an empty state.
 */
static void
initjobs(JobP jobs)
{
	int i;
	for (i = 0; i < MAXJOBS; i++)
		clearjob(&jobs[i]);
}

/*
 * Requires:
 *   "jobs" points to an array of MAXJOBS job structures.
 *
 * Effects:
 *   Returns the largest allocated job ID.
 */
static int
maxjid(JobP jobs) 
{
	int i, max = 0;

	for (i = 0; i < MAXJOBS; i++)
		if (jobs[i].jid > max)
			max = jobs[i].jid;
	return (max);
}

/*
 * Requires:
 *   "jobs" points to an array of MAXJOBS job structures, and "cmdline" is
 *   a properly terminated string.
 *
 * Effects: 
 *   Tries to add a job to the jobs list.  Returns true if the job was added
 *   and false otherwise.
 */
static bool
addjob(JobP jobs, pid_t pid, int state, const char *cmdline)
{
	int i;
    
	if (pid < 1)
		return (false);
	for (i = 0; i < MAXJOBS; i++) {
		if (jobs[i].pid == 0) {
			jobs[i].pid = pid;
			jobs[i].state = state;
			jobs[i].jid = nextjid++;
			if (nextjid > MAXJOBS)
				nextjid = 1;
			// Remove the "volatile" qualifier using a cast.
			strcpy((char *)jobs[i].cmdline, cmdline);
			if (verbose) {
				printf("Added job [%d] %d %s\n", jobs[i].jid,
				    (int)jobs[i].pid, jobs[i].cmdline);
			}
			return (true);
		}
	}
	printf("Tried to create too many jobs\n");
	return (false);
}

/*
 * Requires:
 *   "jobs" points to an array of MAXJOBS job structures.
 *
 * Effects:
 *   Tries to delete the job from the jobs list whose PID equals "pid".
 *   Returns true if the job was deleted and false otherwise.
 */
static bool
deletejob(JobP jobs, pid_t pid) 
{
	int i;

	if (pid < 1)
		return (false);
	for (i = 0; i < MAXJOBS; i++) {
		if (jobs[i].pid == pid) {
			clearjob(&jobs[i]);
			nextjid = maxjid(jobs) + 1;
			return (true);
		}
	}
	return (false);
}

/*
 * Requires:
 *   "jobs" points to an array of MAXJOBS job structures.
 *
 * Effects:
 *   Returns the PID of the current foreground job or 0 if no foreground
 *   job exists.
 */
static pid_t
fgpid(JobP jobs)
{
	int i;

	for (i = 0; i < MAXJOBS; i++)
		if (jobs[i].state == FG)
			return (jobs[i].pid);
	return (0);
}

/*
 * Requires:
 *   "jobs" points to an array of MAXJOBS job structures.
 *
 * Effects:
 *   Returns a pointer to the job structure with process ID "pid" or NULL if
 *   no such job exists.
 */
static JobP
getjobpid(JobP jobs, pid_t pid)
{
	int i;

	if (pid < 1)
		return (NULL);
	for (i = 0; i < MAXJOBS; i++)
		if (jobs[i].pid == pid)
			return (&jobs[i]);
	return (NULL);
}

/*
 * Requires:
 *   "jobs" points to an array of MAXJOBS job structures.
 *
 * Effects:
 *   Returns a pointer to the job structure with job ID "jid" or NULL if no
 *   such job exists.
 */
static JobP
getjobjid(JobP jobs, int jid) 
{
	int i;

	if (jid < 1)
		return (NULL);
	for (i = 0; i < MAXJOBS; i++)
		if (jobs[i].jid == jid)
			return (&jobs[i]);
	return (NULL);
}

/*
 * Requires:
 *   Nothing.
 *
 * Effects:
 *   Returns the job ID for the job with process ID "pid" or 0 if no such
 *   job exists.
 */
static int
pid2jid(pid_t pid) 
{
	int i;

	if (pid < 1)
		return (0);
	for (i = 0; i < MAXJOBS; i++)
		if (jobs[i].pid == pid)
			return (jobs[i].jid);
	return (0);
}

/*
 * Requires:
 *   "jobs" points to an array of MAXJOBS job structures.
 *
 * Effects:
 *   Prints the jobs list.
 */
static void
listjobs(JobP jobs) 
{

	int i;

	for (i = 0; i < MAXJOBS; i++) {
		if (jobs[i].pid != 0) {
			printf("[%d] (%d) ", jobs[i].jid, (int)jobs[i].pid);
			switch (jobs[i].state) {
			case BG: 
				printf("Running ");
				break;
			case FG: 
				printf("Foreground ");
				break;
			case ST: 
				printf("Stopped ");
				break;
			default:
				printf("listjobs: Internal error: "
				    "job[%d].state=%d ", i, jobs[i].state);
			}
			printf("%s", jobs[i].cmdline);
		}
	}
}

/*
 * This comment marks the end of the jobs list helper routines.
 */

/*
 * Other helper routines follow.
 */

/*
 * Requires:
 *   Nothing.
 *
 * Effects:
 *   Prints a help message.
 */
static void
usage(void) 
{

	printf("Usage: shell [-hvp]\n");
	printf("   -h   print this message\n");
	printf("   -v   print additional diagnostic information\n");
	printf("   -p   do not emit a command prompt\n");
	exit(1);
}

/*
 * Requires:
 *   "msg" is a properly terminated string.
 *
 * Effects:
 *   Prints a Unix-style error message and terminates the program.
 */
static void
unix_error(const char *msg)
{

	fprintf(stdout, "%s: %s\n", msg, strerror(errno));
	exit(1);
}

/*
 * Requires:
 *   "msg" is a properly terminated string.
 *
 * Effects:
 *   Prints "msg" and terminates the program.
 */
static void
app_error(const char *msg)
{

	fprintf(stdout, "%s\n", msg);
	exit(1);
}

/*
 * Requires:
 *   The character array "s" is sufficiently large to store the ASCII
 *   representation of the long "v" in base "b".
 *
 * Effects:
 *   Converts a long "v" to a base "b" string, storing that string in the
 *   character array "s" (from K&R).  This function can be safely called by
 *   a signal handler.
 */
static void
sio_ltoa(long v, char s[], int b)
{
	int c, i = 0;

	do
		s[i++] = (c = v % b) < 10 ? c + '0' : c - 10 + 'a';
	while ((v /= b) > 0);
	s[i] = '\0';
	sio_reverse(s);
}

/*
 * Requires:
 *   "s" is a properly terminated string.
 *
 * Effects:
 *   Reverses a string (from K&R).  This function can be safely called by a
 *   signal handler.
 */
static void
sio_reverse(char s[])
{
	int c, i, j;

	for (i = 0, j = sio_strlen(s) - 1; i < j; i++, j--) {
		c = s[i];
		s[i] = s[j];
		s[j] = c;
	}
}

/*
 * Requires:
 *   "s" is a properly terminated string.
 *
 * Effects:
 *   Computes and returns the length of the string "s".  This function can be
 *   safely called by a signal handler.
 */
static size_t
sio_strlen(const char s[])
{
	size_t i = 0;

	while (s[i] != '\0')
		i++;
	return (i);
}

/*
 * Requires:
 *   None.
 *
 * Effects:
 *   Prints the long "v" to stdout using only functions that can be safely
 *   called by a signal handler, and returns either the number of characters
 *   printed or -1 if the long could not be printed.
 */
static ssize_t
sio_putl(long v)
{
	char s[128];
    
	sio_ltoa(v, s, 10);
	return (sio_puts(s));
}

/*
 * Requires:
 *   "s" is a properly terminated string.
 *
 * Effects:
 *   Prints the string "s" to stdout using only functions that can be safely
 *   called by a signal handler, and returns either the number of characters
 *   printed or -1 if the string could not be printed.
 */
static ssize_t
sio_puts(const char s[])
{

	return (write(STDOUT_FILENO, s, sio_strlen(s)));
}

/*
 * Requires:
 *   "s" is a properly terminated string.
 *
 * Effects:
 *   Prints the string "s" to stdout using only functions that can be safely
 *   called by a signal handler, and exits the program.
 */
static void
sio_error(const char s[])
{

	sio_puts(s);
	_exit(1);
}

/*
 * Requires:
 *   None.
 *
 * Effects:
 *   Prints the long "v" to stdout using only functions that can be safely
 *   called by a signal handler.  Either returns the number of characters
 *   printed or exits if the long could not be printed.
 */
static ssize_t
Sio_putl(long v)
{
	ssize_t n;
  
	if ((n = sio_putl(v)) < 0)
		sio_error("Sio_putl error");
	return (n);
}

/*
 * Requires:
 *   "s" is a properly terminated string.
 *
 * Effects:
 *   Prints the string "s" to stdout using only functions that can be safely
 *   called by a signal handler.  Either returns the number of characters
 *   printed or exits if the string could not be printed.
 */
static ssize_t
Sio_puts(const char s[])
{
	ssize_t n;
  
	if ((n = sio_puts(s)) < 0)
		sio_error("Sio_puts error");
	return (n);
}

/*
 * Requires:
 *   "s" is a properly terminated string.
 *
 * Effects:
 *   Prints the string "s" to stdout using only functions that can be safely
 *   called by a signal handler, and exits the program.
 */
static void
Sio_error(const char s[])
{

	sio_error(s);
}

// Prevent "unused function" and "unused variable" warnings.
static const void *dummy_ref[] = { Sio_error, Sio_putl, addjob, builtin_cmd,
    deletejob, do_bgfg, dummy_ref, fgpid, getjobjid, getjobpid, listjobs,
    parseline, pid2jid, signame, waitfg };
