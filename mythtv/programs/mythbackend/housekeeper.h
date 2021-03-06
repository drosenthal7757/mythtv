#ifndef HOUSEKEEPER_H_
#define HOUSEKEEPER_H_

#include <QDateTime>

class Scheduler;
class QString;
class HouseKeeper
{
  public:
    HouseKeeper(bool runthread, bool master, Scheduler *lsched = NULL);
   ~HouseKeeper();

    
  protected:
    void RunHouseKeeping(void);
    static void *doHouseKeepingThread(void *param);

    void RunMFD(void);
    static void *runMFDThread(void *param);

  private:

    bool wantToRun(const QString &dbTag, int period, int minhour, int maxhour,
                   bool nowIfPossible = false);
    void updateLastrun(const QString &dbTag);
    QDateTime getLastRun(const QString &dbTag);
    void flushLogs();
    void runFillDatabase();
    void CleanupMyOldRecordings(void);
    void CleanupAllOldInUsePrograms(void);
    void CleanupOrphanedLivetvChains(void);
    void CleanupRecordedTables(void);
    void CleanupProgramListings(void);
    void RunStartupTasks(void);

    bool threadrunning;
    bool filldbRunning;
    bool isMaster;

    Scheduler *sched;
};

#endif
