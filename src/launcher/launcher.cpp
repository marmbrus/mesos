#include <dirent.h>
#include <errno.h>
#include <libgen.h>
#include <stdlib.h>
#include <pwd.h>

#include <iostream>
#include <sstream>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <boost/lexical_cast.hpp>

#include "launcher.hpp"

#include "common/foreach.hpp"
#include "common/string_utils.hpp"

using std::cerr;
using std::cout;
using std::endl;
using std::ostringstream;
using std::string;
using std::vector;

using boost::lexical_cast;

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::launcher;


ExecutorLauncher::ExecutorLauncher(FrameworkID _frameworkId,
                                   const string& _executorUri,
                                   const string& _user,
                                   const string& _workDirectory,
                                   const string& _slavePid,
                                   const string& _mesosHome,
                                   const string& _hadoopHome,
                                   bool _redirectIO,
                                   bool _shouldSwitchUser,
                                   const map<string, string>& _params)
  : frameworkId(_frameworkId), executorUri(_executorUri), user(_user),
    workDirectory(_workDirectory), slavePid(_slavePid), mesosHome(_mesosHome),
    hadoopHome(_hadoopHome), redirectIO(_redirectIO), 
    shouldSwitchUser(_shouldSwitchUser), params(_params)
{}


ExecutorLauncher::~ExecutorLauncher()
{
}


void ExecutorLauncher::run()
{
  createWorkingDirectory();

  // Enter working directory
  if (chdir(workDirectory.c_str()) < 0)
    fatalerror("chdir into framework working directory failed");

  // Redirect output to files in working dir if required
  if (redirectIO) {
    if (freopen("stdout", "w", stdout) == NULL)
      fatalerror("freopen failed");
    if (freopen("stderr", "w", stderr) == NULL)
      fatalerror("freopen failed");
  }

  string executor = fetchExecutor();

  setupEnvironment();

  if (shouldSwitchUser)
    switchUser();
  
  // Execute the executor
  execl(executor.c_str(), executor.c_str(), (char *) NULL);
  // If we get here, the execl call failed
  fatalerror("Could not execute %s", executor.c_str());
}


// Create the executor's working directory and return its path.
void ExecutorLauncher::createWorkingDirectory()
{
  // Split the path into tokens by "/" and make each directory
  vector<string> tokens;
  StringUtils::split(workDirectory, "/", &tokens);
  string dir = "";
  if (workDirectory.find_first_of("/") == 0) // We got an absolute path, so
    dir = "/";                               // keep the leading slash
  foreach (const string& token, tokens) {
    dir += token;
    if (mkdir(dir.c_str(), 0755) < 0 && errno != EEXIST)
      fatalerror("Failed to mkdir %s", dir.c_str());
    dir += "/";
  }
  // TODO: chown the final directory to the framework's user
}


// Download the executor's binary if required and return its path.
string ExecutorLauncher::fetchExecutor()
{
  string executor = executorUri;

  // Some checks to make using the executor in shell commands safe;
  // these should be pushed into the master and reported to the user
  if (executor.find_first_of('\\') != string::npos ||
      executor.find_first_of('\'') != string::npos ||
      executor.find_first_of('\0') != string::npos) {
    fatal("Illegal characters in executor path");
  }

  // Grab the executor from HDFS if its path begins with hdfs://
  // TODO: Enforce some size limits on files we get from HDFS
  if (executor.find("hdfs://") == 0) {
    // Locate Hadoop's bin/hadoop script. If a Hadoop home was given to us by
    // the slave (from the Mesos config file), use that. Otherwise check for
    // a HADOOP_HOME environment variable. Finally, if that doesn't exist,
    // try looking for hadoop on the PATH.
    string hadoopScript;
    if (hadoopHome != "") {
      hadoopScript = hadoopHome + "/bin/hadoop";
    } else if (getenv("HADOOP_HOME") != 0) {
      hadoopScript = string(getenv("HADOOP_HOME")) + "/bin/hadoop";
    } else {
      hadoopScript = "hadoop"; // Look for hadoop on the PATH.
    }
    
    string localFile = string("./") + basename((char *) executor.c_str());
    ostringstream command;
    command << hadoopScript << " fs -copyToLocal '" << executor
            << "' '" << localFile << "'";
    cout << "Downloading executor from " << executor << endl;
    cout << "HDFS command: " << command.str() << endl;

    int ret = system(command.str().c_str());
    if (ret != 0)
      fatal("HDFS copyToLocal failed: return code %d", ret);
    executor = localFile;
    if (chmod(executor.c_str(), S_IRWXU | S_IRGRP | S_IXGRP |
              S_IROTH | S_IXOTH) != 0)
      fatalerror("chmod failed");
  }

  // If the executor was a .tgz, untar it in the work directory. The .tgz
  // expected to contain a single directory. This directory should contain 
  // a program or script called "executor" to run the executor. We chdir
  // into this directory and run the script from in there.
  if (executor.rfind(".tgz") == executor.size() - strlen(".tgz")) {
    string command = "tar xzf '" + executor + "'";
    cout << "Untarring executor: " + command << endl;
    int ret = system(command.c_str());
    if (ret != 0)
      fatal("Untar failed: return code %d", ret);
    // The .tgz should have contained a single directory; find it
    if (DIR *dir = opendir(".")) {
      bool found = false;
      string dirname = "";
      while (struct dirent *ent = readdir(dir)) {
        if (string(".") != ent->d_name && string("..") != ent->d_name) {
          struct stat info;
          if (stat(ent->d_name, &info) == 0) {
            if (S_ISDIR(info.st_mode)) {
              if (found) // Already found a directory earlier
                fatal("Executor .tgz must contain a single directory");
              dirname = ent->d_name;
              found = true;
            }
          } else {
            fatalerror("Stat failed on %s", ent->d_name);
          }
        }
      }
      if (!found) // No directory found
        fatal("Executor .tgz must contain a single directory");
      if (chdir(dirname.c_str()) < 0)
        fatalerror("Chdir failed");
      executor = "./executor";
    } else {
      fatalerror("Failed to list work directory");
    }
  }

  return executor;
}


// Set up environment variables for launching a framework's executor.
void ExecutorLauncher::setupEnvironment()
{
  // Set any environment variables given as env.* params in the ExecutorInfo
  setupEnvVariablesFromParams();

  // Set Mesos environment variables to pass slave ID, framework ID, etc.
  setenv("MESOS_SLAVE_PID", slavePid.c_str(), true);
  setenv("MESOS_FRAMEWORK_ID", frameworkId.c_str(), true);
  
  // Set LIBPROCESS_PORT so that we bind to a random free port.
  setenv("LIBPROCESS_PORT", "0", true);

  // Set MESOS_HOME so that Java and Python executors can find libraries
  if (mesosHome != "") {
    setenv("MESOS_HOME", mesosHome.c_str(), 1);
  }
}


void ExecutorLauncher::setupEnvVariablesFromParams()
{
  foreachpair (const string& key, const string& value, params) {
    if (key.find("env.") == 0) {
      const string& var = key.substr(strlen("env."));
      setenv(var.c_str(), value.c_str(), true);
    }
  }
}


void ExecutorLauncher::switchUser()
{
  struct passwd *passwd;
  if ((passwd = getpwnam(user.c_str())) == NULL)
    fatal("failed to get username information for %s", user.c_str());

  if (setgid(passwd->pw_gid) < 0)
    fatalerror("failed to setgid");

  if (setuid(passwd->pw_uid) < 0)
    fatalerror("failed to setuid");
}


void ExecutorLauncher::setupEnvironmentForLauncherMain()
{
  // Set up environment variables passed through env.* params
  setupEnvironment();

  // Set up Mesos environment variables that launcher_main.cpp will
  // pass as arguments to an ExecutorLauncher there
  setenv("MESOS_FRAMEWORK_ID", frameworkId.c_str(), 1);
  setenv("MESOS_EXECUTOR_URI", executorUri.c_str(), 1);
  setenv("MESOS_USER", user.c_str(), 1);
  setenv("MESOS_WORK_DIRECTORY", workDirectory.c_str(), 1);
  setenv("MESOS_SLAVE_PID", slavePid.c_str(), 1);
  setenv("MESOS_HOME", mesosHome.c_str(), 1);
  setenv("MESOS_HADOOP_HOME", hadoopHome.c_str(), 1);
  setenv("MESOS_REDIRECT_IO", redirectIO ? "1" : "0", 1);
  setenv("MESOS_SWITCH_USER", shouldSwitchUser ? "1" : "0", 1);
}
