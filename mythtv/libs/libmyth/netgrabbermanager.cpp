// qt
#include <QString>
#include <QCoreApplication>
#include <QProcess>
#include <QFile>
#include <QDir>

#include "mythdirs.h"
#include "mythcontext.h"
#include "mythverbose.h"

#include "netgrabbermanager.h"
#include "netutils.h"

#define LOC      QString("NetContent: ")
#define LOC_WARN QString("NetContent, Warning: ")
#define LOC_ERR  QString("NetContent, Error: ")

using namespace std;

// ---------------------------------------------------

GrabberScript::GrabberScript(const QString& title, const QString& image,
              const ArticleType &type, const QString& author,
              const bool& search, const bool& tree,
              const QString& description, const QString& commandline,
              const double& version) :
        m_lock(QMutex::Recursive)
{
    m_title = title;
    m_image = image;
    m_type = type;
    m_author = author;
    m_search = search;
    m_tree = tree;
    m_description = description;
    m_commandline = commandline;
    m_version = version;
}

GrabberScript::~GrabberScript()
{
}

void GrabberScript::run()
{
    QMutexLocker locker(&m_lock);

    QString commandline = m_commandline;
    m_getTree.setReadChannel(QProcess::StandardOutput);

    if (QFile(commandline).exists())
    {
        m_getTree.start(commandline, QStringList() << "-T");
        m_getTree.waitForFinished(900000);
        QDomDocument domDoc;

        if (QProcess::NormalExit != m_getTree.exitStatus())
        {
            VERBOSE(VB_IMPORTANT, LOC_ERR + QString("Internet Content Source "
                           "%1 crashed while grabbing tree.").arg(m_title));
            emit finished();
            return;
        }

        VERBOSE(VB_IMPORTANT, LOC + QString("Internet Content Source %1 "
                           "completed download, beginning processing...").arg(m_title));

        QByteArray result = m_getTree.readAll();

        domDoc.setContent(result, true);
        QDomElement root = domDoc.documentElement();
        QDomElement channel = root.firstChildElement("channel");

        clearTreeItems(m_title);

        while (!channel.isNull())
        {
            parseDBTree(m_title, QString(), QString(), channel, GetType());
            channel = channel.nextSiblingElement("channel");
        }
        markTreeUpdated(this, QDateTime::currentDateTime());
        VERBOSE(VB_IMPORTANT, LOC + QString("Internet Content Source %1 "
                           "completed processing, marking as updated.").arg(m_title));
        emit finished();
    }
    else
        emit finished();
}

void GrabberScript::parseDBTree(const QString &feedtitle, const QString &path,
                                const QString &pathThumb, QDomElement& domElem,
                                const ArticleType &type)
{
    QMutexLocker locker(&m_lock);

    Parse parse;
    ResultItem::resultList articles;

    // File Handling
    QDomElement fileitem = domElem.firstChildElement("item");
    while (!fileitem.isNull())
    {   // Fill the article list...
        articles.append(parse.ParseItem(fileitem));
        fileitem = fileitem.nextSiblingElement("item");
    }

    while (!articles.isEmpty())
    {   // Insert the articles in the DB...
        insertTreeArticleInDB(feedtitle, path,
                       pathThumb, articles.takeFirst(), type);
    }

    // Directory Handling
    QDomElement diritem = domElem.firstChildElement("directory");
    while (!diritem.isNull())
    {
        QDomElement subfolder = diritem;
        QString dirname = diritem.attribute("name");
        QString dirthumb = diritem.attribute("thumbnail");
        dirname.replace("/", "|");
        QString pathToUse;

        if (path.isEmpty())
            pathToUse = dirname;
        else
            pathToUse = QString("%1/%2").arg(path).arg(dirname);

        parseDBTree(feedtitle,
                    pathToUse,
                    dirthumb,
                    subfolder,
                    type);
        diritem = diritem.nextSiblingElement("directory");
    }
}

GrabberManager::GrabberManager() :     m_lock(QMutex::Recursive)
{
    m_updateFreq = (gCoreContext->GetNumSetting(
                       "netsite.updateFreq", 24) * 3600 * 1000);
    m_timer = new QTimer();
    m_runningCount = 0;
    m_refreshAll = false;
    connect( m_timer, SIGNAL(timeout()),
                      this, SLOT(timeout()));
}

GrabberManager::~GrabberManager()
{
    delete m_timer;
}

void GrabberManager::startTimer()
{
    m_timer->start(m_updateFreq);
}

void GrabberManager::stopTimer()
{
    m_timer->stop();
}

void GrabberManager::doUpdate()
{
    GrabberDownloadThread *gdt = new GrabberDownloadThread(this);
    if (m_refreshAll)
       gdt->refreshAll();
    gdt->start(QThread::LowPriority);

    m_timer->start(m_updateFreq);
}

void GrabberManager::timeout()
{
    QMutexLocker locker(&m_lock);
    doUpdate();
}

void GrabberManager::refreshAll()
{
    m_refreshAll = true;
}

GrabberDownloadThread::GrabberDownloadThread(QObject *parent)
{
    m_parent = parent;
    m_refreshAll = false;
}

GrabberDownloadThread::~GrabberDownloadThread()
{
    cancel();
    wait();
}

void GrabberDownloadThread::cancel()
{
    m_mutex.lock();
    qDeleteAll(m_scripts);
    m_scripts.clear();
    m_mutex.unlock();
}

void GrabberDownloadThread::refreshAll()
{
    m_mutex.lock();
    m_refreshAll = true;
    if (!isRunning())
        start();
    m_mutex.unlock();
}

void GrabberDownloadThread::run()
{
    m_scripts = findAllDBTreeGrabbers();
    uint updateFreq = gCoreContext->GetNumSetting(
               "netsite.updateFreq", 24);

    while (m_scripts.count())
    {
        GrabberScript *script = m_scripts.takeFirst();
        if (script && (needsUpdate(script, updateFreq) || m_refreshAll))
        {
            VERBOSE(VB_IMPORTANT, LOC + QString("Internet Content Source %1 Updating...")
                                  .arg(script->GetTitle()));
            script->run();
        }
        delete script;
    }
    emit finished();
    if (m_parent)
        QCoreApplication::postEvent(m_parent, new GrabberUpdateEvent());
}

Search::Search()
    : m_searchProcess(NULL)
{
    m_videoList.clear();
    m_searchtimer = new QTimer();

    m_searchtimer->setSingleShot(true);
}

Search::~Search()
{
    resetSearch();

    delete m_searchProcess;
    m_searchProcess = NULL;

    if (m_searchtimer)
    {
        m_searchtimer->disconnect();
        m_searchtimer->deleteLater();
        m_searchtimer = NULL;
    }
}


void Search::executeSearch(const QString &script, const QString &query, uint pagenum)
{
    resetSearch();

    m_searchProcess = new QProcess();

    connect(m_searchProcess, SIGNAL(finished(int, QProcess::ExitStatus)),
                  SLOT(slotProcessSearchExit(int, QProcess::ExitStatus)));
    connect(m_searchtimer, SIGNAL(timeout()), this, SLOT(slotSearchTimeout()));

    QString cmd = script;

    QStringList args;

    if (pagenum > 1)
    {
        args.append(QString("-p"));
        args.append(QString::number(pagenum));
    }

    args.append("-S");
    args.append(query);

    VERBOSE(VB_GENERAL|VB_EXTRA, LOC + QString("Internet Search Query: %1 %2")
                                        .arg(cmd).arg(args.join(" ")));

    m_searchtimer->start(40 * 1000);
    m_searchProcess->start(cmd, args);
}

void Search::resetSearch()
{
    qDeleteAll(m_videoList);
    m_videoList.clear();
}

void Search::process()
{
    Parse parse;
    m_videoList = parse.parseRSS(m_document);

    QDomNodeList entries = m_document.elementsByTagName("channel");

    if (entries.count() == 0)
    {
        m_numResults = 0;
        m_numReturned = 0;
        m_numIndex = 0;
        return;
    }

    QDomNode itemNode = entries.item(0);

    QDomNode Node = itemNode.namedItem(QString("numresults"));
    if (!Node.isNull())
    {
        m_numResults = Node.toElement().text().toUInt();
    }
    else
    {
        QDomNodeList count = m_document.elementsByTagName("item");

        if (count.count() == 0)
            m_numResults = 0;
        else
            m_numResults = count.count();
    }

    Node = itemNode.namedItem(QString("returned"));
    if (!Node.isNull())
    {
        m_numReturned = Node.toElement().text().toUInt();
    }
    else
    {
        QDomNodeList entries = m_document.elementsByTagName("item");

        if (entries.count() == 0)
            m_numReturned = 0;
        else
            m_numReturned = entries.count();
    }

    Node = itemNode.namedItem(QString("startindex"));
    if (!Node.isNull())
    {
        m_numIndex = Node.toElement().text().toUInt();
    }
    else
        m_numIndex = 0;

}

void Search::slotProcessSearchExit(int exitcode, QProcess::ExitStatus exitstatus)
{
    if (m_searchtimer)
        m_searchtimer->stop();

    if ((exitstatus != QProcess::NormalExit) ||
        (exitcode != 0))
    {
        m_document.setContent(QString());
    }
    else
    {
        VERBOSE(VB_GENERAL|VB_EXTRA, LOC_ERR + "Internet Search Successfully Completed");

        m_data = m_searchProcess->readAllStandardOutput();
        m_document.setContent(m_data, true);
    }

    m_searchProcess->deleteLater();
    m_searchProcess = NULL;
    emit finishedSearch(this);
}

void Search::SetData(QByteArray data)
{
    m_data = data;
    m_document.setContent(m_data, true);

}
void Search::slotSearchTimeout()
{
    VERBOSE(VB_GENERAL|VB_EXTRA, LOC_ERR + "Internet Search Timeout");

    if (m_searchProcess)
    {
        m_searchProcess->close();
        m_searchProcess->deleteLater();
        m_searchProcess = NULL;
    }
    emit searchTimedOut(this);
}

