#include "EventsWidget.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QHeaderView>
#include <QLineEdit>
#include <QInputDialog>
#include <QMessageBox>
#include <QJsonDocument>
#include <QTime>

#include <aseba/common/consts.h>
#include "CustomWidgets.h"
#include "CustomDelegate.h"
#include "NewNamedValueDialog.h"


namespace Aseba {


EventsWidget::EventsWidget(QWidget* parent) : QWidget(parent) {

    auto* eventsDockLayout = new QVBoxLayout;


    m_view = new FixedWidthTableView;
    m_view->setShowGrid(false);
    m_view->verticalHeader()->hide();
    m_view->horizontalHeader()->hide();
    m_view->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_view->setSelectionMode(QAbstractItemView::SingleSelection);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setDragDropMode(QAbstractItemView::InternalMove);
    m_view->setDragEnabled(true);
    m_view->setDropIndicatorShown(true);
    m_view->setItemDelegateForColumn(1, new SpinBoxDelegate(0, ASEBA_MAX_EVENT_ARG_COUNT, this));
    m_view->setMinimumHeight(100);
    m_view->setSecondColumnLongestContent("255###");
    m_view->resizeRowsToContents();
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);

    QHBoxLayout* eventsAddRemoveLayout = new QHBoxLayout;
    eventsAddRemoveLayout->addWidget(new QLabel(tr("<b>Events</b>")));
    eventsAddRemoveLayout->addStretch();
    auto addEventNameButton = new QPushButton(QPixmap(QString(":/images/add.png")), "");
    eventsAddRemoveLayout->addWidget(addEventNameButton);
    m_removeEventButton = new QPushButton(QPixmap(QString(":/images/remove.png")), "");
    m_removeEventButton->setEnabled(false);
    eventsAddRemoveLayout->addWidget(m_removeEventButton);
    m_sendEventButton = new QPushButton(QPixmap(QString(":/images/newmsg.png")), "");
    m_sendEventButton->setEnabled(false);
    eventsAddRemoveLayout->addWidget(m_sendEventButton);

    eventsDockLayout->addLayout(eventsAddRemoveLayout);

    eventsDockLayout->addWidget(m_view, 1);


    addEventNameButton->setToolTip(tr("Add a new event"));
    m_removeEventButton->setToolTip(tr("Remove this event"));
    m_sendEventButton->setToolTip(tr("Send this event"));

    auto* eventsLayout = new QGridLayout;
    eventsLayout->addWidget(new QLabel(tr("<b>Global Events</b>")), 0, 0, 1, 4);
    eventsLayout->addWidget(addEventNameButton, 1, 0);
    eventsLayout->addWidget(m_removeEventButton, 1, 1);
    eventsLayout->addWidget(m_sendEventButton, 1, 2);
    eventsLayout->addWidget(m_view, 2, 0, 1, 4);


    m_logger = new QListWidget;
    m_logger->setMinimumSize(80, 100);
    m_logger->setSelectionMode(QAbstractItemView::NoSelection);
    eventsDockLayout->addWidget(m_logger, 3);
    auto clearLogger = new QPushButton(tr("Clear"));
    eventsDockLayout->addWidget(clearLogger);

    setLayout(eventsDockLayout);


    connect(addEventNameButton, &QPushButton::clicked, this, &EventsWidget::addEvent);
    connect(m_removeEventButton, &QPushButton::clicked, this, &EventsWidget::removeEvent);
    connect(m_sendEventButton, &QPushButton::clicked, this, &EventsWidget::sendSelectedEvent);
    connect(clearLogger, &QPushButton::clicked, m_logger, &QListWidget::clear);

    connect(m_view, &FixedWidthTableView::doubleClicked, this, &EventsWidget::sendEvent);
}

void EventsWidget::setModel(QAbstractItemModel* model) {
    m_view->setModel(model);
    connect(m_view->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            &EventsWidget::eventsSelectionChanged);
}


void EventsWidget::addEvent() {
    QString eventName;
    int eventNbArgs = 0;
    // prompt the user for the named value
    const bool ok = NewNamedValueDialog::getNamedValue(&eventName, &eventNbArgs, 0, ASEBA_MAX_EVENT_ARG_COUNT,
                                                       tr("Add a new event"), tr("Name:"),
                                                       tr("Number of arguments", "For the newly created event"));
    if(ok && !eventName.isEmpty()) {
        Q_EMIT eventAdded(eventName, eventNbArgs);
    }
}

void EventsWidget::removeEvent() {
    auto r = m_view->selectionModel()->selectedRows();
    if(r.empty())
        return;
    auto name = r.first().data(Qt::DisplayRole);
    Q_EMIT eventRemoved(name.toString());
}

void EventsWidget::eventsSelectionChanged() {
    bool isSelected = m_view->selectionModel()->currentIndex().isValid();
    m_removeEventButton->setEnabled(isSelected);
    m_sendEventButton->setEnabled(isSelected);
}

void EventsWidget::sendSelectedEvent() {
    auto r = m_view->selectionModel()->selectedRows(0);
    if(r.empty())
        return;
    sendEvent(r.first());
}
void EventsWidget::sendEvent(const QModelIndex& idx) {
    if(!idx.isValid())
        return;
    QString name = m_view->model()->data(m_view->model()->index(idx.row(), 0)).toString();
    auto count = m_view->model()->data(m_view->model()->index(idx.row(), 1)).toInt();
    QVariantList data;

    if(count > 0) {
        QString argList;
        while(true) {
            bool ok;
            argList = QInputDialog::getText(this, tr("Specify event arguments"),
                                            tr("Please specify the %0 arguments of event %1").arg(count).arg(name),
                                            QLineEdit::Normal, argList, &ok);
            if(ok) {
                QStringList args = argList.split(QRegExp("[\\s,]+"), QString::SkipEmptyParts);
                if(args.size() != count) {
                    QMessageBox::warning(
                        this, tr("Wrong number of arguments"),
                        tr("You gave %0 arguments where event %1 requires %2").arg(args.size()).arg(name).arg(count));
                    continue;
                }
                for(int i = 0; i < args.size(); i++) {
                    data.append(args.at(i).toShort(&ok));
                    if(!ok) {
                        QMessageBox::warning(this, tr("Invalid value"),
                                             tr("Invalid value for argument %0 of event %1").arg(i).arg(name));
                        break;
                    }
                }
                if(ok)
                    break;
            } else
                return;
        }
    }
    Q_EMIT eventEmitted(name, QVariant::fromValue(data));
}


void EventsWidget::onEvents(const mobsya::ThymioNode::VariableMap& events) {
    for(auto it = events.begin(); it != events.end(); ++it) {
        QString arg = QJsonDocument::fromVariant(it.value().value()).toJson(QJsonDocument::Compact);
        QString text = QStringLiteral("%1\n%2: %3").arg(QTime::currentTime().toString("hh:mm:ss.zzz"), it.key(), arg);
        if(m_logger->count() > 50)
            delete m_logger->takeItem(0);
        m_logger->addItem(new QListWidgetItem(QIcon(":/images/info.png"), text));
        m_logger->scrollToBottom();
    }
}

}  // namespace Aseba
