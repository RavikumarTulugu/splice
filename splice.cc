/*
* Copyright(c) 2012  Ravikumar.T All rights reserved.
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
* 
* Authors:
* Ravikumar.T <naidu.trk@gmail.com>
*
*/

struct syscallException : public std::exception
{
	std::string estr; //string form of error returned by strerror_r 
	int 		error; //copy of actual errno 
	const char *what() const throw () { return estr.c_str(); };
	syscallException(void) {};
	~syscallException(void) throw() {};
};

//reissue the syscall if it encounters a EINTR
//return the return value of the syscall to the caller.
#define _eintr(syscall) ({ int _rc; while(( _rc = (syscall)) < 0x0 && (errno == EINTR)); (_rc); })

//reissue the syscall if its interrupted in the middle (EINTR)
//return the return value of the syscall to the caller. 
//copy the location of the error and the error string, errno 
//to the exception and  throw the exception.
#define _except(syscall) ({										\
	int _rc;													\
	char _estring[512] = {'\0'};								\
	_rc = _eintr(syscall);	    								\
	if ( _rc < 0) {				         						\
		syscallException exc;									\
		exc.error = errno;					    				\
		char line[256] = {'\0'};								\
		sprintf(line,"%s:%s:%d:", __FILE__, __FUNCTION__, __LINE__); \
		exc.estr.append(line, strlen(line));					\
		char *_ptr = strerror_r(errno, _estring, sizeof(_estring)); \
		exc.estr += _ptr ? _ptr : "strerror_r() failure"; 		\
		throw exc;    											\
	}															\
	(_rc);														\
})



int
popenCustom(int *popenPipe, pid_t *childPid, const char *command, char **args) 
{
	pid_t child;
	int in[2]; 
	int out[2];
	int err[2];

	int rc;
	rc = _except(pipe(in));
	if (rc < 0) { perror("Unable to open the input pipe");  goto in_error; }

	rc = _except(pipe(out));
	if (rc < 0) { perror("Unable to open the output pipe");  goto out_error; }

	rc = _except(pipe(err));
	if (rc < 0) { perror("Unable to open the error pipe");  goto err_error; }

	child = fork();
	if (child) { //parent
		_except(close(in[0]));
		_except(close(out[1]));
		_except(close(err[1]));

		popenPipe[0] = in[1];
		popenPipe[1] = out[0];
		popenPipe[2] = err[0];

		*childPid = child;

		//since the fds will be used with select, its better we set them
		//nonblocking mode. 
		rc = _except(fcntl(popenPipe[1], F_SETFL, O_NONBLOCK));
		if (rc < 0) { perror("Unable to set the popenPipe[1] to nonblocking ");  goto execv_error; }
		rc = _except(fcntl(popenPipe[2], F_SETFL, O_NONBLOCK));
		if (rc < 0) { perror("Unable to set the popenPipe[2] to nonblocking ");  goto execv_error; }

	} else if(child == 0) { //child
		_except(close(in[1]));
		_except(close(out[0]));
		_except(close(err[0]));

		int rc;
		_except(close(0)); 
		_except(dup(in[0]));
		_except(close(1)); 
		_except(dup(out[1]));
		_except(close(2)); 
		_except(dup(err[1]));

		rc = execv(command, args); //finally spawn the command 
		if (rc < 0) { 
			perror("execvp call failure "); 
			goto execv_error; 
		}
	} else { 
		perror("fork failure:"); 
		goto fork_error; 
	}

	return 0; 

execv_error:
fork_error:
	_except(close(err[1]));
	_except(close(err[0]));

err_error:
	_except(close(out[1]));
	_except(close(out[0]));

out_error:
	_except(close(in[1]));
	_except(close(in[0]));

in_error:
	return -1;
}

//call the command with the arguments
//stdout_callback is invoked with the contents put on stdout by child 
//stderr_callback is invoked with the contents put on stderr by child 
//both the callbacks are passed the fd to the child_stdin so that they 
//can write to the child input basing on the contents of child's stdout
//or stderr.
int 
spawnCommand(int *popenPipe, const char *command, char **args, int *childExitCode,
		void(*stdout_callback)(int child_stdin, const char *buf, size_t size),
		void(*stderr_callback)(int child_stdin, const char *buf, size_t size)
		)
{
	int childStatus;
	int rc = 0;

	pid_t child;
	if(popenCustom(popenPipe, &child, command, args) < 0)  return -1;

	//listen on the stdout and stderr of the child
	fd_set childOutFds, workingSet;
	FD_ZERO(&childOutFds);
	FD_SET(popenPipe[1], &childOutFds);
	FD_SET(popenPipe[2], &childOutFds);
	int max = popenPipe[1] > popenPipe[2] ? popenPipe[1] : popenPipe[2];

	while (1) {
		memset(&workingSet, 0, sizeof(fd_set));
		memcpy(&workingSet, &childOutFds, sizeof(fd_set));

		int rc = 0;
		struct timeval tv = {120, 0}; //2 min is significantly a long timeout
		rc = _except(select(max+1, &workingSet, NULL, NULL, &tv));
		if (rc < 0) {
			perror("select failed:");
			break;
		} else if (rc) {
			if(FD_ISSET(popenPipe[1], &workingSet)) {
				int rc = 0;
				//Try to drain the pipe 
				do {
					char childOutput[2048] = {'\0'};
					rc = _except(read(popenPipe[1], childOutput, sizeof(childOutput)));
					if (rc < 0) {
						if ((errno != EAGAIN) || (errno != EWOULDBLOCK)) {
							perror("read on childout failed"); 
							return -1;
						}
					}
					else if (rc) {
						if (stdout_callback)
							stdout_callback(popenPipe[0], childOutput, rc); 
					}
					//other end has exited and read returned 0 bytes here 
					//just close the pipes and get the fuck out of here 
					else goto collect_wait_status;
				} while(rc > 0);
			}

			if(FD_ISSET(popenPipe[2], &workingSet)) {
				int rc = 0;
				//Try to drain the pipe
				do {
					char childOutput[2048] = {'\0'};
					rc = _except(read(popenPipe[2], childOutput, sizeof(childOutput)));
					if (rc < 0) {
						if ((errno != EAGAIN) || (errno != EWOULDBLOCK)) {
							perror("read on childerr failed");
							return -1;
						}
					}
					else if (rc) {
						if (stderr_callback)
							stderr_callback(popenPipe[0], childOutput, rc); 
					}
					//other end has exited and read returned 0 bytes here 
					//just close the pipes and get the fuck out of here 
					else goto collect_wait_status;
				} while(rc > 0);
			}
		} else {
			int rc = 0;
			rc = _except(waitpid(child, &childStatus, WNOHANG));
			if (rc == 0) continue;  //move on with select
			else {
				if (rc < 0) perror("waitpid failed");
				goto closefds_and_exit;
			}
		}
	}

	//close the pipe descriptors and return
collect_wait_status:
	rc = _except(waitpid(child, &childStatus, 0));
	assert(rc); //we are here coz of the child exit 
				//so the return value of waitpid has 
				//to be nonzero.
closefds_and_exit:
	_except(close(popenPipe[0]));
	_except(close(popenPipe[1]));
	_except(close(popenPipe[2]));
	*childExitCode = WEXITSTATUS(childStatus);
	return 0;
}

int 
pcloseCustom(int *rwePipe)
{
	_except(close(rwePipe[0]));
	_except(close(rwePipe[1]));
	_except(close(rwePipe[2]));
	return 0;
}
