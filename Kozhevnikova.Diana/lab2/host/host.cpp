#include <QApplication>
#include <sys/syslog.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <cstring>
#include <unistd.h>
#include <semaphore.h>
#include <csignal>
#include <thread>

#include "host.h"

Host::Host() {
    struct sigaction sig{};
    memset(&sig, 0, sizeof(sig));
    sig.sa_flags = SA_SIGINFO;
    sig.sa_sigaction = Host::SignalHandler;
    sigaction(SIGTERM, &sig, nullptr);
    sigaction(SIGINT, &sig, nullptr);
}

void Host::SignalHandler(int signum, siginfo_t* info, void *ptr) {
    switch (signum) {
    case SIGTERM:
        Host::getInstance().isRunning = false;
        return;
    case SIGINT:
        syslog(LOG_INFO, "host terminate");
        exit(EXIT_SUCCESS);
        return;
    default:
        syslog(LOG_INFO, "unknown command");
    }
}

void Host::run() {
    syslog(LOG_INFO, "Host-chat started");
    if (prepare() == false) {
        stop();
        return;
    }
    std::thread connThread(&Host::connectionWork, this);

    std::string winName = "Host";
    int argc = 1;
    char* args[] = { (char*)winName.c_str()};
    QApplication app(argc, args);
    ChatWin window(winName, writeWin, readWin, IsRun);
    window.show();
    app.exec();
    
    stop();
    connThread.join();
}

void Host::stop() {
    if (isRunning.load()) {
        syslog(LOG_INFO, "Chat-host: stop working");
        isRunning = false;
    }
}

bool Host::prepare() {
    syslog(LOG_INFO, "Chat-host-conn: start init");
    hostPid = getpid();
    conn = Connection::create(hostPid, true);
    hostSem = sem_open("/Host-sem", O_CREAT, S_IRWXU | S_IRWXG | S_IRWXO, 0);
    if (hostSem == SEM_FAILED) {
        syslog(LOG_ERR, "ERROR: host semaphore not created");
        return false;
    }
    clientSem = sem_open("/Client-sem", O_CREAT, S_IRWXU | S_IRWXG | S_IRWXO, 0);
    if (clientSem == SEM_FAILED) {
        sem_close(hostSem);
        syslog(LOG_ERR, "ERROR: client semaphore not created");
        return false;
    }
    try {
        Connection *raw = conn.get();
        raw->open(hostPid, true);
    }
    catch (std::exception &e) {
        syslog(LOG_ERR, "ERROR: %s", e.what());
        sem_close(hostSem);
        sem_close(clientSem);
        return false;
    }

    pid_t childPid = fork();
    if (childPid == 0)
    {
        clientPid = getpid();

        if (Client::getInstance().init(hostPid))
            Client::getInstance().run();
        else 
        {
            syslog(LOG_ERR, "ERROR: client initialization error");
            return false;
        }
        exit(EXIT_SUCCESS);
    }

    Host::getInstance().isRunning = true;
    syslog(LOG_INFO, "INFO: host initialize successfully");
    return true;
}

void Host::connectionWork() {
    lastMsgTime = std::chrono::high_resolution_clock::now();

    while (isRunning.load()) {
        double minutes_passed = std::chrono::duration_cast<std::chrono::minutes>(
            std::chrono::high_resolution_clock::now() - lastMsgTime).count();

        if (minutes_passed >= 1) {
          syslog(LOG_INFO, "INFO [Host]: Killing chat for 1 minute silence");
          isRunning = false;
          break;
        }
        if (!readMsg())
          break;
        if (!writeMsg())
          break;

        std::this_thread::sleep_for(std::chrono::milliseconds(60));
    }
    connectionClose();
}


bool Host::readMsg() {
    {
        timespec t;
        clock_gettime(CLOCK_REALTIME, &t);
        t.tv_sec += 5;
        int s = sem_timedwait(hostSem, &t);
        if (s == -1)
        {
            syslog(LOG_ERR, "ERROR [Host]: Read semaphore timeout");
            isRunning = false;
            return false;
        }
    }

    if (messagesIn.pushConnection(conn.get()) == false)
    {
        isRunning = false;
        return false;
    }
    else if (messagesIn.getSize() > 0)
        lastMsgTime = std::chrono::high_resolution_clock::now();

    return true;
}

bool Host::writeMsg() {
    bool res = messagesOut.popConnection(conn.get());
    sem_post(clientSem);
    return res;
}

void Host::connectionClose() {
    conn->close();
    sem_close(hostSem);
    sem_close(clientSem);
    kill(clientPid, SIGTERM);
}

bool Host::IsRun() {
    return Host::getInstance().isRunning.load();
}
    
bool Host::readWin(Message *msg) {
    return Host::getInstance().messagesIn.popMessage(msg);
}

void Host::writeWin(Message msg) {
    Host::getInstance().messagesOut.pushMessage(msg);
}

int main(int argc, char *argv[]) {
    openlog("Chat log", LOG_NDELAY | LOG_PID, LOG_USER);
    try {
        Host::getInstance().run();
    } catch (std::exception &e) {
        syslog(LOG_ERR, "ERROR: %s. Close chat...", e.what());
    }
    closelog();
    return 0;
}