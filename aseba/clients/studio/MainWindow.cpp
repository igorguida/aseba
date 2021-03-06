/*
    Aseba - an event-based framework for distributed robot control
    Created by Stéphane Magnenat <stephane at magnenat dot net> (http://stephane.magnenat.net)
    with contributions from the community.
    Copyright (C) 2007--2018 the authors, see authors.txt for details.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation, version 3 of the License.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "MainWindow.h"
#include "ClickableLabel.h"
#include "TargetModels.h"
#include "NamedValuesVectorModel.h"
#include "StudioAeslEditor.h"
#include "EventViewer.h"
#include "FindDialog.h"
#include "ModelAggregator.h"
#include "translations/CompilerTranslator.h"
#include "common/consts.h"
#include "common/productids.h"
#include "common/utils/utils.h"
#include "common/about/AboutDialog.h"
#include "NodeTab.h"
#include "NodeTabsManager.h"

#include <QtGui>
#include <QtWidgets>
#include <QtXml>
#include <sstream>
#include <iostream>
#include <cassert>
#include <QTabWidget>
#include <QDesktopServices>
#include <QSvgRenderer>
#include <utility>
#include <iostream>

using std::copy;


namespace Aseba {
/** \addtogroup studio */
/*@{*/

CompilationLogDialog::CompilationLogDialog(QWidget* parent) : QDialog(parent), te(new QTextEdit()) {
    auto* l(new QVBoxLayout);
    l->addWidget(te);
    setLayout(l);

    QFont font;
    font.setFamily(QLatin1String(""));
    font.setStyleHint(QFont::TypeWriter);
    font.setFixedPitch(true);
    font.setPointSize(10);

    te->setFont(font);
    te->setTabStopWidth(QFontMetrics(font).width(' ') * 4);
    te->setReadOnly(true);

    setWindowTitle(tr("Aseba Studio: Output of last compilation"));

    resize(600, 560);
}

void CompilationLogDialog::hideEvent(QHideEvent* event) {
    if(!isVisible())
        emit hidden();
}

MainWindow::MainWindow(const mobsya::ThymioDeviceManagerClient& client, const QVector<QUuid>& targetUuids,
                       QWidget* parent)
    : QMainWindow(parent) {

    nodes = new NodeTabsManager(client);
    connect(nodes, &NodeTabsManager::tabAdded, this, &MainWindow::tabAdded);

    // create help viwer
    helpViewer.setupWidgets();
    helpViewer.setupConnections();

    // create config dialog + read settings on-disk
    ConfigDialog::init(this);

    // create gui
    setupWidgets();
    setupMenu();
    setupConnections();
    setWindowIcon(QIcon(":/images/icons/asebastudio.svgz"));

    // cosmetic fix-up
    updateWindowTitle();
    if(readSettings() == false)
        resize(1000, 700);

    for(auto&& id : targetUuids) {
        nodes->addTab(id);
    }
}

MainWindow::~MainWindow() {}


void MainWindow::tabAdded(int index) {
    NodeTab* tab = qobject_cast<NodeTab*>(nodes->widget(index));
    if(!tab)
        return;
    connect(showHiddenAct, &QAction::toggled, tab, &NodeTab::showHidden);
    tab->showHidden(showHiddenAct->isChecked());
}

void MainWindow::about() {
    const AboutBox::Parameters aboutParameters = {
        "Aseba Studio",
        ":/images/icons/asebastudio.svgz",
        tr("Aseba Studio is an environment for interactively programming robots with a text "
           "language."),
        tr("https://www.thymio.org/en:asebastudio"),
        "",
        {"core", "studio", "vpl", "packaging", "translation"}};
    AboutBox aboutBox(this, aboutParameters);
    aboutBox.exec();
}

bool MainWindow::newFile() {
    if(askUserBeforeDiscarding()) {
        // clear content
        clearDocumentSpecificTabs();
        // we must only have NodeTab* left, clear content of editors in tabs
        for(int i = 0; i < nodes->count(); i++) {
            auto* tab = dynamic_cast<NodeTab*>(nodes->widget(i));
            Q_ASSERT(tab);
            tab->editor->clear();
        }

        // reset opened file name
        clearOpenedFileName(false);
        return true;
    }
    return false;
}

void MainWindow::openFile(const QString& path) {
    // make sure we do not loose changes
    if(askUserBeforeDiscarding() == false)
        return;

    // open the file
    QString fileName = path;

    // if no file to open is passed, show a dialog
    if(fileName.isEmpty()) {
        // try to guess the directory of the last opened or saved file
        QString dir;
        if(!actualFileName.isEmpty()) {
            // a document is opened, propose to open it again
            dir = actualFileName;
        } else {
            // no document is opened, try recent files
            QSettings settings;
            QStringList recentFiles = settings.value(QStringLiteral("recent files")).toStringList();
            if(recentFiles.size() > 0) {
                dir = recentFiles[0];
            } else {
                const QStringList stdLocations(QStandardPaths::standardLocations(QStandardPaths::DocumentsLocation));
                dir = !stdLocations.empty() ? stdLocations[0] : QLatin1String("");
            }
        }

        fileName = QFileDialog::getOpenFileName(this, tr("Open Script"), dir, QStringLiteral("Aseba scripts (*.aesl)"));
    }

    QFile file(fileName);
    if(!file.open(QFile::ReadOnly))
        return;

    // load the document
    QDomDocument document(QStringLiteral("aesl-source"));
    QString errorMsg;
    int errorLine;
    int errorColumn;
    if(document.setContent(&file, false, &errorMsg, &errorLine, &errorColumn)) {
        // remove event and constant definitions
        // eventsDescriptionsModel->clear();
        // constantsDefinitionsModel->clear();
        // delete all absent node tabs
        clearDocumentSpecificTabs();
        // we must only have NodeTab* left, clear content of editors in tabs
        for(int i = 0; i < nodes->count(); i++) {
            auto* tab = dynamic_cast<NodeTab*>(nodes->widget(i));
            Q_ASSERT(tab);
            tab->editor->clear();
        }

        // build list of tabs filled from file to be loaded
        QSet<int> filledList;
        QDomNode domNode = document.documentElement().firstChild();
        while(!domNode.isNull()) {
            if(domNode.isElement()) {
                QDomElement element = domNode.toElement();
                /*if(element.tagName() == "node") {
                    bool prefered;
                    NodeTab* tab = getTabFromName(element.attribute("name"),
                                                  element.attribute("nodeId", nullptr).toUInt(), &prefered);
                    if(prefered) {
                        const int index(nodes->indexOf(tab));
                        assert(index >= 0);
                        filledList.insert(index);
                    }
                }*/
            }
            domNode = domNode.nextSibling();
        }

        // load file
        int noNodeCount = 0;
        actualFileName = fileName;
        domNode = document.documentElement().firstChild();
        while(!domNode.isNull()) {
            if(domNode.isElement()) {
                QDomElement element = domNode.toElement();
                if(element.tagName() == QLatin1String("node")) {
                    // load plugins xml data

                    // get text
                    QString text;
                    for(QDomNode n = element.firstChild(); !n.isNull(); n = n.nextSibling()) {
                        QDomText t = n.toText();
                        if(!t.isNull())
                            text += t.data();
                    }

                    // reconstruct nodes
                    bool prefered;
                    const QString nodeName(element.attribute(QStringLiteral("name")));
                    const unsigned nodeId(element.attribute(QStringLiteral("nodeId"), nullptr).toUInt());
                    NodeTab* tab = nullptr;  // getTabFromName(nodeName, nodeId, &prefered, &filledList);
                    if(tab) {
                        // matching tab name
                        if(prefered) {
                            // the node is the prefered one, fill now
                            tab->editor->setPlainText(text);
                            // note that the node is already marked in filledList
                        } else {
                            const int index(nodes->indexOf(tab));
                            // the node is not filled, fill now
                            tab->editor->setPlainText(text);
                            filledList.insert(index);
                        }
                    } else {
                        // no matching name or no free slot, create an absent tab
                        // nodes->addTab(new AbsentNodeTab(nodeId, nodeName, text, savedPlugins),
                        //              nodeName + tr(" (not available)"));
                        noNodeCount++;
                    }
                } /*else if(element.tagName() == "event") {
                    const QString eventName(element.attribute("name"));
                    const unsigned eventSize(element.attribute("size").toUInt());
                    eventsDescriptionsModel->addNamedValue(
                        NamedValue(eventName.toStdWString(), std::min(unsigned(ASEBA_MAX_EVENT_ARG_SIZE), eventSize)));
                } else if(element.tagName() == "constant") {
                    constantsDefinitionsModel->addNamedValue(
                        NamedValue(element.attribute("name").toStdWString(), element.attribute("value").toInt()));
                } else if(element.tagName() == "keywords") {
                    if(element.attribute("flag") == "true")
                        showKeywordsAct->setChecked(true);
                    else
                        showKeywordsAct->setChecked(false);
                }*/
            }
            domNode = domNode.nextSibling();
        }

        // check if there was some matching problem
        if(noNodeCount)
            QMessageBox::warning(this, tr("Loading"),
                                 tr("%0 scripts have no corresponding nodes in the current network "
                                    "and have not been loaded.")
                                     .arg(noNodeCount));

        // update recent files
        updateRecentFiles(fileName);
        regenerateOpenRecentMenu();

        updateWindowTitle();
    } else {
        QMessageBox::warning(
            this, tr("Loading"),
            tr("Error in XML source file: %0 at line %1, column %2").arg(errorMsg).arg(errorLine).arg(errorColumn));
    }

    file.close();
}

void MainWindow::openRecentFile() {
    auto* entry = dynamic_cast<QAction*>(sender());
    openFile(entry->text());
}

bool MainWindow::save() {
    return saveFile(actualFileName);
}

bool MainWindow::saveFile(const QString& previousFileName) {
    return false;


    /*    QString fileName = previousFileName;

        if(fileName.isEmpty())
            fileName = QFileDialog::getSaveFileName(
                this, tr("Save Script"),
                actualFileName.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) :
                                           actualFileName,
                "Aseba scripts (*.aesl)");

        if(fileName.isEmpty())
            return false;

        if(fileName.lastIndexOf(".") < 0)
            fileName += ".aesl";

        QFile file(fileName);
        if(!file.open(QFile::WriteOnly | QFile::Truncate))
            return false;

        actualFileName = fileName;
        updateRecentFiles(fileName);

        // initiate DOM tree
        QDomDocument document("aesl-source");
        QDomElement root = document.createElement("network");
        document.appendChild(root);

        root.appendChild(document.createTextNode("\n\n\n"));
        root.appendChild(document.createComment("list of global events"));

        // events
        for(size_t i = 0; i < commonDefinitions.events.size(); i++) {
            QDomElement element = document.createElement("event");
            element.setAttribute("name", QString::fromStdWString(commonDefinitions.events[i].name));
            element.setAttribute("size", QString::number(commonDefinitions.events[i].value));
            root.appendChild(element);
        }

        root.appendChild(document.createTextNode("\n\n\n"));
        root.appendChild(document.createComment("list of constants"));

        // constants
        for(size_t i = 0; i < commonDefinitions.constants.size(); i++) {
            QDomElement element = document.createElement("constant");
            element.setAttribute("name", QString::fromStdWString(commonDefinitions.constants[i].name));
            element.setAttribute("value", QString::number(commonDefinitions.constants[i].value));
            root.appendChild(element);
        }

        // keywords
        root.appendChild(document.createTextNode("\n\n\n"));
        root.appendChild(document.createComment("show keywords state"));

        QDomElement keywords = document.createElement("keywords");
        if(showKeywordsAct->isChecked())
            keywords.setAttribute("flag", "true");
        else
            keywords.setAttribute("flag", "false");
        root.appendChild(keywords);

        // source code
        for(int i = 0; i < nodes->count(); i++) {
            const auto* tab = dynamic_cast<const ScriptTab*>(nodes->widget(i));
            if(tab) {
                QString nodeName;

                const auto* nodeTab = dynamic_cast<const NodeTab*>(tab);
                if(nodeTab)
                    nodeName = target->getName(nodeTab->nodeId());

                const auto* absentNodeTab = dynamic_cast<const AbsentNodeTab*>(tab);
                if(absentNodeTab)
                    nodeName = absentNodeTab->name;

                const QString& nodeContent = tab->editor->toPlainText();
                ScriptTab::SavedPlugins savedPlugins(tab->savePlugins());
                // is there something to save?
                if(!nodeContent.isEmpty() || !savedPlugins.isEmpty()) {
                    root.appendChild(document.createTextNode("\n\n\n"));
                    root.appendChild(document.createComment(QString("node %0").arg(nodeName)));

                    QDomElement element = document.createElement("node");
                    element.setAttribute("name", nodeName);
                    element.setAttribute("nodeId", tab->nodeId());
                    QDomText text = document.createTextNode(nodeContent);
                    element.appendChild(text);
                    if(!savedPlugins.isEmpty()) {
                        QDomElement plugins = document.createElement("toolsPlugins");
                        for(ScriptTab::SavedPlugins::const_iterator it(savedPlugins.begin()); it != savedPlugins.end();
                            ++it) {
                            const NodeToolInterface::SavedContent content(*it);
                            QDomElement plugin(document.createElement(content.first));
                            plugin.appendChild(document.importNode(content.second.documentElement(), true));
                            plugins.appendChild(plugin);
                        }
                        element.appendChild(plugins);
                    }
                    root.appendChild(element);
                }
            }
        }
        root.appendChild(document.createTextNode("\n\n\n"));

        QTextStream out(&file);
        document.save(out, 0);

        sourceModified = false;
        constantsDefinitionsModel->clearWasModified();
        eventsDescriptionsModel->clearWasModified();
        updateWindowTitle();

        return true;
    */
}

void MainWindow::exportMemoriesContent() {
    /*    QString exportFileName = QFileDialog::getSaveFileName(
            this, tr("Export memory content"), QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
            "All Files (*);;CSV files (*.csv);;Text files (*.txt)");

        QFile file(exportFileName);
        if(!file.open(QFile::WriteOnly | QFile::Truncate))
            return;

        QTextStream out(&file);

        for(int i = 0; i < nodes->count(); i++) {
            auto* tab = dynamic_cast<NodeTab*>(nodes->widget(i));
            if(tab) {
                const QString nodeName(target->getName(tab->nodeId()));
                const QList<TargetVariablesModel::Variable>& variables(tab->vmMemoryModel->getVariables());

                for(int j = 0; j < variables.size(); ++j) {
                    const TargetVariablesModel::Variable& variable(variables[j]);
                    out << nodeName << "." << variable.name << " ";
                    for(size_t k = 0; k < variable.value.size(); ++k) {
                        out << variable.value[k] << " ";
                    }
                    out << "\n";
                }
            }
        }
    */
}

void MainWindow::copyAll() {
    /*    QString toCopy;
        for(int i = 0; i < nodes->count(); i++) {
            const NodeTab* nodeTab = dynamic_cast<NodeTab*>(nodes->widget(i));
            if(nodeTab) {
                toCopy += QString("# node %0\n").arg(target->getName(nodeTab->nodeId()));
                toCopy += nodeTab->editor->toPlainText();
                toCopy += "\n\n";
            }
            const AbsentNodeTab* absentNodeTab = dynamic_cast<AbsentNodeTab*>(nodes->widget(i));
            if(absentNodeTab) {
                toCopy += QString("# absent node named %0\n").arg(absentNodeTab->name);
                toCopy += absentNodeTab->editor->toPlainText();
                toCopy += "\n\n";
            }
        }
        QApplication::clipboard()->setText(toCopy);
    */
}

void MainWindow::findTriggered() {
    auto* tab = dynamic_cast<ScriptTab*>(nodes->currentWidget());
    if(tab && tab->editor->textCursor().hasSelection())
        findDialog->setFindText(tab->editor->textCursor().selectedText());
    findDialog->replaceGroupBox->setChecked(false);
    findDialog->show();
}

void MainWindow::replaceTriggered() {
    findDialog->replaceGroupBox->setChecked(true);
    findDialog->show();
}

void MainWindow::commentTriggered() {
    if(!currentScriptTab)
        return;
    currentScriptTab->editor->commentAndUncommentSelection(AeslEditor::CommentSelection);
}

void MainWindow::uncommentTriggered() {
    if(!currentScriptTab)
        return;
    currentScriptTab->editor->commentAndUncommentSelection(AeslEditor::UncommentSelection);
}

void MainWindow::showLineNumbersChanged(bool state) {
    for(int i = 0; i < nodes->count(); i++) {
        auto* tab = dynamic_cast<NodeTab*>(nodes->widget(i));
        Q_ASSERT(tab);
        tab->linenumbers->showLineNumbers(state);
    }
    ConfigDialog::setShowLineNumbers(state);
}

void MainWindow::goToLine() {
    if(!currentScriptTab)
        return;
    QTextEdit* editor(currentScriptTab->editor);
    const QTextDocument* document(editor->document());
    QTextCursor cursor(editor->textCursor());
    bool ok;
    const int curLine = cursor.blockNumber() + 1;
    const int minLine = 1;
    const int maxLine = document->lineCount();
    const int line = QInputDialog::getInt(this, tr("Go To Line"), tr("Line:"), curLine, minLine, maxLine, 1, &ok);
    if(ok)
        editor->setTextCursor(QTextCursor(document->findBlockByLineNumber(line - 1)));
}

void MainWindow::zoomIn() {
    if(!currentScriptTab)
        return;
    QTextEdit* editor(currentScriptTab->editor);
    editor->zoomIn();
}

void MainWindow::zoomOut() {
    if(!currentScriptTab)
        return;
    QTextEdit* editor(currentScriptTab->editor);
    editor->zoomOut();
}

void MainWindow::showSettings() {
    ConfigDialog::showConfig();
}

void MainWindow::toggleBreakpoint() {
    if(!currentScriptTab)
        return;
    currentScriptTab->editor->toggleBreakpoint();
}

void MainWindow::clearAllBreakpoints() {
    if(!currentScriptTab)
        return;
    currentScriptTab->editor->clearAllBreakpoints();
}

void MainWindow::resetAll() {
    for(int i = 0; i < nodes->count(); i++) {
        auto* tab = dynamic_cast<NodeTab*>(nodes->widget(i));
        if(tab)
            tab->reset();
    }
}

void MainWindow::runAll() {
    for(int i = 0; i < nodes->count(); i++) {
        auto* tab = dynamic_cast<NodeTab*>(nodes->widget(i));
        if(tab)
            tab->run();
    }
}

void MainWindow::pauseAll() {
    for(int i = 0; i < nodes->count(); i++) {
        auto* tab = dynamic_cast<NodeTab*>(nodes->widget(i));
        if(tab)
            tab->run();
    }
}

void MainWindow::stopAll() {
    /*    for(int i = 0; i < nodes->count(); i++) {
            auto* tab = dynamic_cast<NodeTab*>(nodes->widget(i));
            if(tab)
                target->stop(tab->nodeId());
        }
    */
}

void MainWindow::clearAllExecutionError() {
    for(int i = 0; i < nodes->count(); i++) {
        auto* tab = dynamic_cast<NodeTab*>(nodes->widget(i));
        if(tab)
            tab->clearExecutionErrors();
    }
    logger->setStyleSheet(QLatin1String(""));
}

void MainWindow::uploadReadynessChanged() {
    /* bool ready = true;
     for(int i = 0; i < nodes->count(); i++) {
         auto* tab = dynamic_cast<NodeTab*>(nodes->widget(i));
         if(tab) {
             if(!tab->loadButton->isEnabled()) {
                 ready = false;
                 break;
             }
         }
     }

     loadAllAct->setEnabled(ready);
     writeAllBytecodesAct->setEnabled(ready);
     */
}

void MainWindow::toggleEventVisibleButton(const QModelIndex& index) {
    // if(index.column() == 2)
    //    eventsDescriptionsModel->toggle(index);
}

void MainWindow::plotEvent() {
#ifdef HAVE_QWT
    QModelIndex currentRow = eventsDescriptionsView->selectionModel()->currentIndex();
    Q_ASSERT(currentRow.isValid());
    const unsigned eventId = currentRow.row();
    plotEvent(eventId);
#endif  // HAVE_QWT
}

void MainWindow::eventContextMenuRequested(const QPoint& pos) {
#ifdef HAVE_QWT
    const QModelIndex index(eventsDescriptionsView->indexAt(pos));
    if(index.isValid() && (index.column() == 0)) {
        const QString eventName(eventsDescriptionsModel->data(index).toString());
        QMenu menu;
        menu.addAction(tr("Plot event %1").arg(eventName));
        const QAction* ret = menu.exec(eventsDescriptionsView->mapToGlobal(pos));
        if(ret) {
            const unsigned eventId = index.row();
            plotEvent(eventId);
        }
    }
#endif  // HAVE_QWT
}

void MainWindow::plotEvent(const unsigned eventId) {
#ifdef HAVE_QWT
    const unsigned eventVariablesCount(
        eventsDescriptionsModel->data(eventsDescriptionsModel->index(eventId, 1)).toUInt());
    const QString eventName(eventsDescriptionsModel->data(eventsDescriptionsModel->index(eventId, 0)).toString());
    const QString tabTitle(tr("plot of %1").arg(eventName));
    nodes->addTab(new EventViewer(eventId, eventName, eventVariablesCount, &eventsViewers), tabTitle, true);
#endif  // HAVE_QWT
}

void MainWindow::logEntryDoubleClicked(QListWidgetItem* item) {
    if(item->data(Qt::UserRole).type() == QVariant::Point) {
        int node = item->data(Qt::UserRole).toPoint().x();
        int line = item->data(Qt::UserRole).toPoint().y();

        NodeTab* tab = nullptr;  // getTabFromId(node);
        Q_ASSERT(tab);
        nodes->setCurrentWidget(tab);
        tab->editor->setTextCursor(QTextCursor(tab->editor->document()->findBlockByLineNumber(line)));
        tab->editor->setFocus();
    }
}

void MainWindow::tabChanged(int index) {
    findDialog->hide();
    auto* tab = dynamic_cast<ScriptTab*>(nodes->widget(index));
    if(currentScriptTab && tab != currentScriptTab) {
        disconnect(cutAct, &QAction::triggered, currentScriptTab->editor, &QTextEdit::cut);
        disconnect(copyAct, &QAction::triggered, currentScriptTab->editor, &QTextEdit::copy);
        disconnect(pasteAct, &QAction::triggered, currentScriptTab->editor, &QTextEdit::paste);
        disconnect(undoAct, &QAction::triggered, currentScriptTab->editor, &QTextEdit::undo);
        disconnect(redoAct, &QAction::triggered, currentScriptTab->editor, &QTextEdit::redo);
        disconnect(currentScriptTab->editor, &QTextEdit::copyAvailable, cutAct, &QAction::setEnabled);
        disconnect(currentScriptTab->editor, &QTextEdit::copyAvailable, copyAct, &QAction::setEnabled);
        disconnect(currentScriptTab->editor, &QTextEdit::undoAvailable, undoAct, &QAction::setEnabled);
        disconnect(currentScriptTab->editor, &QTextEdit::redoAvailable, redoAct, &QAction::setEnabled);
    }
    currentScriptTab = tab;
    if(tab) {
        connect(cutAct, &QAction::triggered, currentScriptTab->editor, &QTextEdit::cut);
        connect(copyAct, &QAction::triggered, currentScriptTab->editor, &QTextEdit::copy);
        connect(pasteAct, &QAction::triggered, currentScriptTab->editor, &QTextEdit::paste);
        connect(undoAct, &QAction::triggered, currentScriptTab->editor, &QTextEdit::undo);
        connect(redoAct, &QAction::triggered, currentScriptTab->editor, &QTextEdit::redo);
        connect(currentScriptTab->editor, &QTextEdit::copyAvailable, cutAct, &QAction::setEnabled);
        connect(currentScriptTab->editor, &QTextEdit::copyAvailable, copyAct, &QAction::setEnabled);
        connect(currentScriptTab->editor, &QTextEdit::undoAvailable, undoAct, &QAction::setEnabled);
        connect(currentScriptTab->editor, &QTextEdit::redoAvailable, redoAct, &QAction::setEnabled);

        findDialog->editor = tab->editor;
    } else {
        findDialog->editor = nullptr;
    }

    cutAct->setEnabled(currentScriptTab);
    copyAct->setEnabled(currentScriptTab);
    pasteAct->setEnabled(currentScriptTab);
    findAct->setEnabled(currentScriptTab);
    undoAct->setEnabled(currentScriptTab);
    redoAct->setEnabled(currentScriptTab);
    goToLineAct->setEnabled(currentScriptTab);
    zoomInAct->setEnabled(currentScriptTab);
    zoomOutAct->setEnabled(currentScriptTab);
    findDialog->replaceGroupBox->setEnabled(currentScriptTab);
}

void MainWindow::showCompilationMessages(bool doShow) {
    // this slot shouldn't be callable when an unactive tab is show
    compilationMessageBox->setVisible(doShow);
    if(nodes->currentWidget())
        dynamic_cast<NodeTab*>(nodes->currentWidget())->compileCodeOnTarget();
}

void MainWindow::compilationMessagesWasHidden() {
    showCompilationMsg->setChecked(false);
}

void MainWindow::showMemoryUsage(bool show) {
    for(int i = 0; i < nodes->count(); i++) {
        auto* tab = dynamic_cast<NodeTab*>(nodes->widget(i));
        if(tab)
            tab->showMemoryUsage(show);
    }
    ConfigDialog::setShowMemoryUsage(show);
}


void MainWindow::resetStatusText() {
    /*bool flag = true;

    for(int i = 0; i < nodes->count(); i++) {
        auto* tab = dynamic_cast<NodeTab*>(nodes->widget(i));
        if(tab) {
            if(!tab->isSynchronized) {
                flag = false;
                break;
            }
        }
    }

    if(flag) {
        statusText->clear();
        statusText->hide();
    }*/
}

void MainWindow::recompileAll() {
    for(int i = 0; i < nodes->count(); i++) {
        auto* tab = dynamic_cast<NodeTab*>(nodes->widget(i));
        if(tab)
            tab->compileCodeOnTarget();
    }
}

void MainWindow::writeAllBytecodes() {
    for(int i = 0; i < nodes->count(); i++) {
        auto* tab = dynamic_cast<NodeTab*>(nodes->widget(i));
        // if(tab)
        //    tab->wr();
    }
}

void MainWindow::rebootAllNodes() {
    for(int i = 0; i < nodes->count(); i++) {
        auto* tab = dynamic_cast<NodeTab*>(nodes->widget(i));
        if(tab)
            tab->reboot();
    }
}

void MainWindow::sourceChanged() {
    updateWindowTitle();
}

void MainWindow::showUserManual() {
    helpViewer.showHelp(HelpViewer::USERMANUAL);
}

void MainWindow::clearDocumentSpecificTabs() {
    /*bool changed = false;
    do {
        changed = false;
        for(int i = 0; i < nodes->count(); i++) {
            QWidget* tab = nodes->widget(i);

#ifdef HAVE_QWT
            if(dynamic_cast<AbsentNodeTab*>(tab) || dynamic_cast<EventViewer*>(tab))
#else   // HAVE_QWT
            if(dynamic_cast<AbsentNodeTab*>(tab))
#endif  // HAVE_QWT
            {
                nodes->removeAndDeleteTab(i);
                changed = true;
                break;
            }
        }
    } while(changed);*/
}

void MainWindow::setupWidgets() {
    currentScriptTab = nullptr;
    nodes->setSizePolicy(QSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding));

    auto* splitter = new QSplitter();
    splitter->addWidget(nodes);
    setCentralWidget(splitter);

    /*
    // setColumnStretch

    */

    /*QHBoxLayout* constantsAddRemoveLayout = new QHBoxLayout;
    ;
    constantsAddRemoveLayout->addStretch();
    addConstantButton = new QPushButton(QPixmap(QString(":/images/add.png")), "");
    constantsAddRemoveLayout->addWidget(addConstantButton);
    removeConstantButton = new QPushButton(QPixmap(QString(":/images/remove.png")), "");
    removeConstantButton->setEnabled(false);
    constantsAddRemoveLayout->addWidget(removeConstantButton);
    */

    // eventsDockLayout->addLayout(constantsAddRemoveLayout);
    // eventsDockLayout->addWidget(constantsView, 1);


#ifdef HAVE_QWT
    plotEventButton = new QPushButton(QPixmap(QString(":/images/plot.png")), "");
    plotEventButton->setEnabled(false);
    plotEventButton->setToolTip(tr("Plot this event"));
#endif  // HAVE_QWT


    // dialog box
    compilationMessageBox = new CompilationLogDialog(this);
    connect(this, &MainWindow::MainWindowClosed, compilationMessageBox, &QWidget::close);
    findDialog = new FindDialog(this);
    connect(this, &MainWindow::MainWindowClosed, findDialog, &QWidget::close);

    connect(this, &MainWindow::MainWindowClosed, &helpViewer, &QWidget::close);
}

void MainWindow::setupConnections() {
    // general connections
    connect(nodes, &QTabWidget::currentChanged, this, &MainWindow::tabChanged);
    // connect(logger, &QListWidget::itemDoubleClicked, this, &MainWindow::logEntryDoubleClicked);
    connect(ConfigDialog::getInstance(), &ConfigDialog::settingsChanged, this, &MainWindow::applySettings);

    // global actions
    connect(loadAllAct, SIGNAL(triggered()), SLOT(loadAll()));
    connect(resetAllAct, &QAction::triggered, this, &MainWindow::resetAll);
    connect(runAllAct, &QAction::triggered, this, &MainWindow::runAll);
    connect(pauseAllAct, &QAction::triggered, this, &MainWindow::pauseAll);

    // events
    // connect(addEventNameButton, &QAbstractButton::clicked, this, &MainWindow::addEventNameClicked);
    // connect(removeEventNameButton, &QAbstractButton::clicked, this, &MainWindow::removeEventNameClicked);
    // connect(sendEventButton, &QAbstractButton::clicked, this, &MainWindow::sendEvent);
#ifdef HAVE_QWT
    connect(plotEventButton, SIGNAL(clicked()), SLOT(plotEvent()));
#endif  // HAVE_QWT
    /*connect(eventsDescriptionsView->selectionModel(),
            SIGNAL(selectionChanged(const QItemSelection&, const QItemSelection&)),
            SLOT(eventsDescriptionsSelectionChanged()));
    connect(eventsDescriptionsView, SIGNAL(doubleClicked(const QModelIndex&)), SLOT(sendEventIf(const QModelIndex&)));
    connect(eventsDescriptionsView, SIGNAL(clicked(const QModelIndex&)),
            SLOT(toggleEventVisibleButton(const QModelIndex&)));
    connect(eventsDescriptionsModel, SIGNAL(dataChanged(const QModelIndex&, const QModelIndex&)),
            SLOT(eventsUpdated()));
    connect(eventsDescriptionsModel, SIGNAL(publicRowsInserted()), SLOT(eventsUpdated()));
    connect(eventsDescriptionsModel, SIGNAL(publicRowsRemoved()), SLOT(eventsUpdatedDirty()));
    connect(eventsDescriptionsView, SIGNAL(customContextMenuRequested(const QPoint&)),
            SLOT(eventContextMenuRequested(const QPoint&)));*/

    // logger
    // connect(clearLogger, &QAbstractButton::clicked, logger, &QListWidget::clear);
    // connect(clearLogger, &QAbstractButton::clicked, this, &MainWindow::clearAllExecutionError);

    // constants
    // connect(addConstantButton, &QAbstractButton::clicked, this, &MainWindow::addConstantClicked);
    // connect(removeConstantButton, &QAbstractButton::clicked, this, &MainWindow::removeConstantClicked);
    // connect(constantsView->selectionModel(), SIGNAL(selectionChanged(const QItemSelection&, const QItemSelection&)),
    //        SLOT(constantsSelectionChanged()));
    // connect(constantsDefinitionsModel, SIGNAL(dataChanged(const QModelIndex&, const QModelIndex&)),
    //         SLOT(recompileAll()));
    // connect(constantsDefinitionsModel, SIGNAL(dataChanged(const QModelIndex&, const QModelIndex&)),
    //       SLOT(updateWindowTitle()));
    /*
        // target events
        connect(target, SIGNAL(nodeConnected(unsigned)), SLOT(nodeConnected(unsigned)));
        connect(target, SIGNAL(nodeDisconnected(unsigned)), SLOT(nodeDisconnected(unsigned)));

        connect(target, SIGNAL(userEvent(unsigned, const VariablesDataVector&)),
                SLOT(userEvent(unsigned, const VariablesDataVector&)));
        connect(target, SIGNAL(userEventsDropped(unsigned)), SLOT(userEventsDropped(unsigned)));
        connect(target, SIGNAL(arrayAccessOutOfBounds(unsigned, unsigned, unsigned, unsigned)),
                SLOT(arrayAccessOutOfBounds(unsigned, unsigned, unsigned, unsigned)));
        connect(target, SIGNAL(divisionByZero(unsigned, unsigned)), SLOT(divisionByZero(unsigned, unsigned)));
        connect(target, SIGNAL(eventExecutionKilled(unsigned, unsigned)), SLOT(eventExecutionKilled(unsigned,
       unsigned))); connect(target, SIGNAL(nodeSpecificError(unsigned, unsigned, QString)),
                SLOT(nodeSpecificError(unsigned, unsigned, QString)));

        connect(target, SIGNAL(executionPosChanged(unsigned, unsigned)), SLOT(executionPosChanged(unsigned,
       unsigned))); connect(target, SIGNAL(executionModeChanged(unsigned, Target::ExecutionMode)),
                SLOT(executionModeChanged(unsigned, Target::ExecutionMode)));
        connect(target, SIGNAL(variablesMemoryEstimatedDirty(unsigned)),
       SLOT(variablesMemoryEstimatedDirty(unsigned)));

        connect(target, SIGNAL(variablesMemoryChanged(unsigned, unsigned, const VariablesDataVector&)),
                SLOT(variablesMemoryChanged(unsigned, unsigned, const VariablesDataVector&)));

        connect(target, SIGNAL(breakpointSetResult(unsigned, unsigned, bool)),
                SLOT(breakpointSetResult(unsigned, unsigned, bool)));
    */
}

void MainWindow::regenerateOpenRecentMenu() {
    openRecentMenu->clear();

    // Add all other actions excepted the one we are processing
    QSettings settings;
    QStringList recentFiles = settings.value(QStringLiteral("recent files")).toStringList();
    for(int i = 0; i < recentFiles.size(); i++) {
        const QString& fileName(recentFiles.at(i));
        openRecentMenu->addAction(fileName, this, SLOT(openRecentFile()));
    }
}

void MainWindow::updateRecentFiles(const QString& fileName) {
    QSettings settings;
    QStringList recentFiles = settings.value(QStringLiteral("recent files")).toStringList();
    if(recentFiles.contains(fileName))
        recentFiles.removeAt(recentFiles.indexOf(fileName));
    recentFiles.push_front(fileName);
    const int maxRecentFiles = 8;
    if(recentFiles.size() > maxRecentFiles)
        recentFiles.pop_back();
    settings.setValue(QStringLiteral("recent files"), recentFiles);
}

void MainWindow::regenerateToolsMenus() {
    writeBytecodeMenu->clear();
    rebootMenu->clear();
    unsigned activeVMCount(0);
    for(int i = 0; i < nodes->count(); i++) {
        auto* tab = qobject_cast<NodeTab*>(nodes->widget(i));
        if(tab && tab->thymio()) {
            QAction* act = writeBytecodeMenu->addAction(tr("...inside %0").arg(tab->thymio()->name()));
            connect(tab, SIGNAL(uploadReadynessChanged(bool)), act, SLOT(setEnabled(bool)));
            rebootMenu->addAction(tr("...%0").arg(tab->thymio()->name()), tab, SLOT(reboot()));
            connect(tab, SIGNAL(uploadReadynessChanged(bool)), act, SLOT(setEnabled(bool)));
            ++activeVMCount;
        }
    }
    writeBytecodeMenu->addSeparator();
    writeAllBytecodesAct = writeBytecodeMenu->addAction(tr("...inside all nodes"), this, SLOT(writeAllBytecodes()));
    rebootMenu->addSeparator();
    rebootMenu->addAction(tr("...all nodes"), this, SLOT(rebootAllNodes()));
    globalToolBar->setVisible(activeVMCount > 1);
}

void MainWindow::generateHelpMenu() {
    helpMenu->addAction(tr("&User Manual..."), this, SLOT(showUserManual()), QKeySequence::HelpContents);
    helpMenu->addSeparator();

    helpMenuTargetSpecificSeparator = helpMenu->addSeparator();
    helpMenu->addAction(tr("Web site Aseba..."), this, SLOT(openToUrlFromAction()))
        ->setData(QUrl(tr("http://aseba.wikidot.com/en:start")));
    helpMenu->addAction(tr("Report bug..."), this, SLOT(openToUrlFromAction()))
        ->setData(QUrl(tr("http://github.com/mobsya/aseba/issues/new")));

#ifdef Q_WS_MAC
    helpMenu->addAction("about", this, SLOT(about()));
    helpMenu->addAction("About &Qt...", qApp, SLOT(aboutQt()));
#else   // Q_WS_MAC
    helpMenu->addSeparator();
    helpMenu->addAction(tr("&About..."), this, SLOT(about()));
    helpMenu->addAction(tr("About &Qt..."), qApp, SLOT(aboutQt()));
#endif  // Q_WS_MAC
}

void MainWindow::regenerateHelpMenu() {
    // remove old target-specific actions
    while(!targetSpecificHelp.isEmpty()) {
        QAction* action(targetSpecificHelp.takeFirst());
        helpMenu->removeAction(action);
        delete action;
    }

    // add back target-specific actions
    using ProductIds = std::set<int>;
    ProductIds productIds;
    for(int i = 0; i < nodes->count(); i++) {
        auto* tab = dynamic_cast<NodeTab*>(nodes->widget(i));
        if(tab)
            productIds.insert(tab->productId());
    }
    for(auto it(productIds.begin()); it != productIds.end(); ++it) {
        QAction* action;
        switch(*it) {
            case ASEBA_PID_THYMIO2:
                action = new QAction(tr("Thymio programming tutorial..."), helpMenu);
                connect(action, &QAction::triggered, this, &MainWindow::openToUrlFromAction);
                action->setData(QUrl(tr("http://aseba.wikidot.com/en:thymiotutoriel")));
                targetSpecificHelp.append(action);
                helpMenu->insertAction(helpMenuTargetSpecificSeparator, action);
                action = new QAction(tr("Thymio programming interface..."), helpMenu);
                connect(action, &QAction::triggered, this, &MainWindow::openToUrlFromAction);
                action->setData(QUrl(tr("http://aseba.wikidot.com/en:thymioapi")));
                targetSpecificHelp.append(action);
                helpMenu->insertAction(helpMenuTargetSpecificSeparator, action);
                break;

            case ASEBA_PID_CHALLENGE:
                action = new QAction(tr("Challenge tutorial..."), helpMenu);
                connect(action, &QAction::triggered, this, &MainWindow::openToUrlFromAction);
                action->setData(QUrl(tr("http://aseba.wikidot.com/en:gettingstarted")));
                targetSpecificHelp.append(action);
                helpMenu->insertAction(helpMenuTargetSpecificSeparator, action);
                break;

            case ASEBA_PID_MARXBOT:
                action = new QAction(tr("MarXbot user manual..."), helpMenu);
                connect(action, &QAction::triggered, this, &MainWindow::openToUrlFromAction);
                action->setData(QUrl(tr("http://mobots.epfl.ch/data/robots/marxbot-user-manual.pdf")));
                targetSpecificHelp.append(action);
                helpMenu->insertAction(helpMenuTargetSpecificSeparator, action);
                break;

            default: break;
        }
    }
}

void MainWindow::openToUrlFromAction() const {
    const QAction* action(reinterpret_cast<QAction*>(sender()));
    QDesktopServices::openUrl(action->data().toUrl());
}

void MainWindow::setupMenu() {
    // File menu
    QMenu* fileMenu = new QMenu(tr("&File"), this);
    menuBar()->addMenu(fileMenu);

    fileMenu->addAction(QIcon(":/images/filenew.png"), tr("&New"), this, SLOT(newFile()), QKeySequence::New);
    fileMenu->addAction(QIcon(":/images/fileopen.png"), tr("&Open..."), this, SLOT(openFile()), QKeySequence::Open);
    openRecentMenu = new QMenu(tr("Open &Recent"), fileMenu);
    regenerateOpenRecentMenu();
    fileMenu->addMenu(openRecentMenu)->setIcon(QIcon(":/images/fileopen.png"));

    fileMenu->addAction(QIcon(":/images/filesave.png"), tr("&Save..."), this, SLOT(save()), QKeySequence::Save);
    fileMenu->addAction(QIcon(":/images/filesaveas.png"), tr("Save &As..."), this, SLOT(saveFile()),
                        QKeySequence::SaveAs);

    fileMenu->addSeparator();
    fileMenu->addAction(QIcon(":/images/filesaveas.png"), tr("Export &memories content..."), this,
                        SLOT(exportMemoriesContent()));

    fileMenu->addSeparator();
#ifdef Q_WS_MAC
    fileMenu->addAction(QIcon(":/images/exit.png"), "quit", this, SLOT(close()), QKeySequence::Quit);
#else   // Q_WS_MAC
    fileMenu->addAction(QIcon(":/images/exit.png"), tr("&Quit"), this, SLOT(close()), QKeySequence::Quit);
#endif  // Q_WS_MAC

    // Edit menu
    cutAct = new QAction(QIcon(":/images/editcut.png"), tr("Cu&t"), this);
    cutAct->setShortcut(QKeySequence::Cut);
    cutAct->setEnabled(false);

    copyAct = new QAction(QIcon(":/images/editcopy.png"), tr("&Copy"), this);
    copyAct->setShortcut(QKeySequence::Copy);
    copyAct->setEnabled(false);

    pasteAct = new QAction(QIcon(":/images/editpaste.png"), tr("&Paste"), this);
    pasteAct->setShortcut(QKeySequence::Paste);
    pasteAct->setEnabled(false);

    undoAct = new QAction(QIcon(":/images/undo.png"), tr("&Undo"), this);
    undoAct->setShortcut(QKeySequence::Undo);
    undoAct->setEnabled(false);

    redoAct = new QAction(QIcon(":/images/redo.png"), tr("Re&do"), this);
    redoAct->setShortcut(QKeySequence::Redo);
    redoAct->setEnabled(false);

    findAct = new QAction(QIcon(":/images/find.png"), tr("&Find..."), this);
    findAct->setShortcut(QKeySequence::Find);
    connect(findAct, &QAction::triggered, this, &MainWindow::findTriggered);
    findAct->setEnabled(false);

    replaceAct = new QAction(QIcon(":/images/edit.png"), tr("&Replace..."), this);
    replaceAct->setShortcut(QKeySequence::Replace);
    connect(replaceAct, &QAction::triggered, this, &MainWindow::replaceTriggered);
    replaceAct->setEnabled(false);

    goToLineAct = new QAction(QIcon(":/images/goto.png"), tr("&Go To Line..."), this);
    goToLineAct->setShortcut(tr("Ctrl+G", "Edit|Go To Line"));
    goToLineAct->setEnabled(false);
    connect(goToLineAct, &QAction::triggered, this, &MainWindow::goToLine);

    commentAct = new QAction(tr("Comment the selection"), this);
    commentAct->setShortcut(tr("Ctrl+D", "Edit|Comment the selection"));
    connect(commentAct, &QAction::triggered, this, &MainWindow::commentTriggered);

    uncommentAct = new QAction(tr("Uncomment the selection"), this);
    uncommentAct->setShortcut(tr("Shift+Ctrl+D", "Edit|Uncomment the selection"));
    connect(uncommentAct, &QAction::triggered, this, &MainWindow::uncommentTriggered);

    QMenu* editMenu = new QMenu(tr("&Edit"), this);
    menuBar()->addMenu(editMenu);
    editMenu->addAction(cutAct);
    editMenu->addAction(copyAct);
    editMenu->addAction(pasteAct);
    editMenu->addSeparator();
    editMenu->addAction(QIcon(":/images/editcopy.png"), tr("Copy &all"), this, SLOT(copyAll()));
    editMenu->addSeparator();
    editMenu->addAction(undoAct);
    editMenu->addAction(redoAct);
    editMenu->addSeparator();
    editMenu->addAction(findAct);
    editMenu->addAction(replaceAct);
    editMenu->addSeparator();
    editMenu->addAction(goToLineAct);
    editMenu->addSeparator();
    editMenu->addAction(commentAct);
    editMenu->addAction(uncommentAct);

    // View menu
    showMemoryUsageAct = new QAction(tr("Show &memory usage"), this);
    showMemoryUsageAct->setCheckable(true);
    connect(showMemoryUsageAct, &QAction::toggled, this, &MainWindow::showMemoryUsage);

    showHiddenAct = new QAction(tr("S&how hidden variables and functions"), this);
    showHiddenAct->setCheckable(true);

    showLineNumbers = new QAction(tr("Show &Line Numbers"), this);
    showLineNumbers->setShortcut(tr("F11", "View|Show Line Numbers"));
    showLineNumbers->setCheckable(true);
    connect(showLineNumbers, &QAction::toggled, this, &MainWindow::showLineNumbersChanged);

    zoomInAct = new QAction(tr("&Increase font size"), this);
    zoomInAct->setShortcut(QKeySequence::ZoomIn);
    zoomInAct->setEnabled(false);
    connect(zoomInAct, &QAction::triggered, this, &MainWindow::zoomIn);

    zoomOutAct = new QAction(tr("&Decrease font size"), this);
    zoomOutAct->setShortcut(QKeySequence::ZoomOut);
    zoomOutAct->setEnabled(false);
    connect(zoomOutAct, &QAction::triggered, this, &MainWindow::zoomOut);

    QMenu* viewMenu = new QMenu(tr("&View"), this);
    viewMenu->addAction(showMemoryUsageAct);
    viewMenu->addAction(showHiddenAct);
    viewMenu->addAction(showLineNumbers);
    viewMenu->addSeparator();
    viewMenu->addAction(zoomInAct);
    viewMenu->addAction(zoomOutAct);
    viewMenu->addSeparator();
#ifdef Q_WS_MAC
    viewMenu->addAction("settings", this, SLOT(showSettings()), QKeySequence::Preferences);
#else   // Q_WS_MAC
    viewMenu->addAction(tr("&Settings"), this, SLOT(showSettings()), QKeySequence::Preferences);
#endif  // Q_WS_MAC
    menuBar()->addMenu(viewMenu);

    // Debug actions
    loadAllAct = new QAction(QIcon(":/images/upload.png"), tr("&Load all"), this);
    loadAllAct->setShortcut(tr("F7", "Load|Load all"));

    resetAllAct = new QAction(QIcon(":/images/reset.png"), tr("&Reset all"), this);
    resetAllAct->setShortcut(tr("F8", "Debug|Reset all"));

    runAllAct = new QAction(QIcon(":/images/play.png"), tr("Ru&n all"), this);
    runAllAct->setShortcut(tr("F9", "Debug|Run all"));

    pauseAllAct = new QAction(QIcon(":/images/pause.png"), tr("&Pause all"), this);
    pauseAllAct->setShortcut(tr("F10", "Debug|Pause all"));

    // Debug toolbar
    globalToolBar = addToolBar(tr("Debug"));
    globalToolBar->setObjectName(QStringLiteral("debug toolbar"));
    globalToolBar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    globalToolBar->addAction(loadAllAct);
    globalToolBar->addAction(resetAllAct);
    globalToolBar->addAction(runAllAct);
    globalToolBar->addAction(pauseAllAct);

    // Debug menu
    toggleBreakpointAct = new QAction(tr("Toggle breakpoint"), this);
    toggleBreakpointAct->setShortcut(tr("Ctrl+B", "Debug|Toggle breakpoint"));
    connect(toggleBreakpointAct, &QAction::triggered, this, &MainWindow::toggleBreakpoint);

    clearAllBreakpointsAct = new QAction(tr("Clear all breakpoints"), this);
    // clearAllBreakpointsAct->setShortcut();
    connect(clearAllBreakpointsAct, &QAction::triggered, this, &MainWindow::clearAllBreakpoints);

    QMenu* debugMenu = new QMenu(tr("&Debug"), this);
    menuBar()->addMenu(debugMenu);
    debugMenu->addAction(toggleBreakpointAct);
    debugMenu->addAction(clearAllBreakpointsAct);
    debugMenu->addSeparator();
    debugMenu->addAction(loadAllAct);
    debugMenu->addAction(resetAllAct);
    debugMenu->addAction(runAllAct);
    debugMenu->addAction(pauseAllAct);

    // Tool menu
    QMenu* toolMenu = new QMenu(tr("&Tools"), this);
    menuBar()->addMenu(toolMenu);
    /*toolMenu->addAction(QIcon(":/images/view_text.png"), tr("&Show last compilation messages"),
                        this, SLOT(showCompilationMessages()),
                        QKeySequence(tr("Ctrl+M", "Tools|Show last compilation messages")));*/
    showCompilationMsg = new QAction(QIcon(":/images/view_text.png"), tr("&Show last compilation messages"), this);
    showCompilationMsg->setCheckable(true);
    toolMenu->addAction(showCompilationMsg);
    connect(showCompilationMsg, &QAction::toggled, this, &MainWindow::showCompilationMessages);
    connect(compilationMessageBox, &CompilationLogDialog::hidden, this, &MainWindow::compilationMessagesWasHidden);
    toolMenu->addSeparator();
    writeBytecodeMenu = new QMenu(tr("Write the program(s)..."), toolMenu);
    toolMenu->addMenu(writeBytecodeMenu);
    rebootMenu = new QMenu(tr("Reboot..."), toolMenu);
    toolMenu->addMenu(rebootMenu);

    // Help menu
    helpMenu = new QMenu(tr("&Help"), this);
    menuBar()->addMenu(helpMenu);
    generateHelpMenu();
    regenerateHelpMenu();

    // add dynamic stuff
    regenerateToolsMenus();

    // Load the state from the settings (thus from hard drive)
    applySettings();
}
//! Ask the user to save or discard or ignore the operation that would destroy the unmodified data.
/*!
    \return true if it is ok to discard, false if operation must abort
*/
bool MainWindow::askUserBeforeDiscarding() {
    const bool anythingModified = false;
    //    sourceModified || constantsDefinitionsModel->checkIfModified() || eventsDescriptionsModel->checkIfModified();
    if(anythingModified == false)
        return true;

    QString docName(tr("Untitled"));
    if(!actualFileName.isEmpty())
        docName = actualFileName.mid(actualFileName.lastIndexOf(QLatin1String("/")) + 1);

    QMessageBox msgBox;
    msgBox.setWindowTitle(tr("Aseba Studio - Confirmation Dialog"));
    msgBox.setText(tr("The document \"%0\" has been modified.").arg(docName));
    msgBox.setInformativeText(tr("Do you want to save your changes or discard them?"));
    msgBox.setStandardButtons(QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    msgBox.setDefaultButton(QMessageBox::Save);

    int ret = msgBox.exec();
    switch(ret) {
        case QMessageBox::Save:
            // Save was clicked
            if(save())
                return true;
            else
                return false;
        case QMessageBox::Discard:
            // Don't Save was clicked
            return true;
        case QMessageBox::Cancel:
            // Cancel was clicked
            return false;
        default:
            // should never be reached
            assert(false);
            break;
    }

    return false;
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if(askUserBeforeDiscarding()) {
        writeSettings();
        event->accept();
        emit MainWindowClosed();
    } else {
        event->ignore();
    }
}

bool MainWindow::readSettings() {
    bool result;

    QSettings settings;
    result = restoreGeometry(settings.value(QStringLiteral("MainWindow/geometry")).toByteArray());
    restoreState(settings.value(QStringLiteral("MainWindow/windowState")).toByteArray());
    return result;
}

void MainWindow::writeSettings() {
    QSettings settings;
    settings.setValue(QStringLiteral("MainWindow/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("MainWindow/windowState"), saveState());
}

void MainWindow::updateWindowTitle() {
    const bool anythingModified = false;
    //   sourceModified || constantsDefinitionsModel->checkIfModified() ||
    //   eventsDescriptionsModel->checkIfModified();

    QString modifiedText;
    if(anythingModified)
        modifiedText = tr("[modified] ");

    QString docName(tr("Untitled"));
    if(!actualFileName.isEmpty())
        docName = actualFileName.mid(actualFileName.lastIndexOf(QLatin1String("/")) + 1);

    setWindowTitle(tr("%0 %1- Aseba Studio").arg(docName).arg(modifiedText));
}

void MainWindow::applySettings() {
    showMemoryUsageAct->setChecked(ConfigDialog::getShowMemoryUsage());
    showHiddenAct->setChecked(ConfigDialog::getShowHidden());
    showLineNumbers->setChecked(ConfigDialog::getShowLineNumbers());
}

void MainWindow::clearOpenedFileName(bool isModified) {
    actualFileName.clear();
    updateWindowTitle();
}

/*@}*/
}  // namespace Aseba
