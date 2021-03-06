#ifndef CHANNELGROUPSETTINGS_H
#define CHANNELGROUPSETTINGS_H

#include "settings.h"

class MPUBLIC ChannelGroupConfig: public ConfigurationWizard
{
 public:
    ChannelGroupConfig(QString _name);
    QString getName(void) const { return name; }

 private:
    QString name;
};

class MPUBLIC ChannelGroupEditor : public QObject, public ConfigurationDialog
{
    Q_OBJECT

  public:
    ChannelGroupEditor(void);
    virtual DialogCode exec(void);
    virtual void Load(void);
    virtual void Save(void) { };
    virtual void Save(QString) { };
    virtual MythDialog* dialogWidget(MythMainWindow* parent,
                                     const char* widgetName=0);

  protected slots:
    void open(QString name);
    void doDelete(void);

  protected:
    ListBoxSetting *listbox;
    QString         lastValue;
};

#endif
