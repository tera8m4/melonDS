#include "JSBreakPointManager.h"
#include "RAMInfoDialog.h"
#include "qapplication.h"
#include "qclipboard.h"
#include "qdebug.h"
#include "EmuThread.h"
#include <QTextCodec>
#include <QByteArray>
#include <QJSEngine>
#include <QFile>
#include <QTimer>
#include <QFileInfo>
#include <cstdint>

namespace {
auto readMemoryAddr(EmuThread* emuThread, uint32_t addr) {
    auto num = emuThread->NDS->ARM9Read32(addr);
    auto b0 = (num & 0x000000ff) << 24u;
    auto b1 = (num & 0x0000ff00) << 8u;
    auto b2 = (num & 0x00ff0000) >> 8u;
    auto b3 = (num & 0xff000000) >> 24u;

    return b0 | b1 | b2 | b3;
}
}

JSBreakPointManager::JSBreakPointManager(QObject *parent, EmuThread* emuThread)
    : QObject{parent},
    emuThread{emuThread}
{
    connect(emuThread, &EmuThread::onBreakPoint, this, &JSBreakPointManager::onBreakPoint);
    jsEngine = new QJSEngine(this);

    timer = new QTimer(this);
    connect(timer, SIGNAL(timeout()),
            this, SLOT(onTimerUpdate()));
    timer->start(500);
}

void JSBreakPointManager::loadScript(const QString &inLocation)
{
    reset();

    qDebug() << "load script" << inLocation << "\n";
    QFile file(inLocation);
    file.open(QFile::ReadOnly);
    jsEngine->evaluate(file.readAll(), inLocation);
    loadedFile = inLocation;
}

void JSBreakPointManager::log(const QString &msg) const
{
    qDebug() << "JSBreakPointManager: " << msg << "\n";
}

void JSBreakPointManager::registerBreakPoint(const int addr, QJSValue breakPointCallback)
{
    breakPointCallbacks[addr] = breakPointCallback;

    emuThread->addBreakPoint(addr);
}

void JSBreakPointManager::onBreakPoint(int addr)
{
    auto it = breakPointCallbacks.find(addr);
    if (it != breakPointCallbacks.end()) {

        auto result = it->call(QJSValueList());
        if (result.isError()) {
            qDebug() 
                << "Uncaught exception at line"
                << result.property("lineNumber").toInt()
                << ":" << result.toString();
        }
    }
}

void JSBreakPointManager::continueExecution()
{
    emuThread->continueExecution();
}

int JSBreakPointManager::readMemotyByte(int addr)
{
    const auto value = emuThread->NDS->ARM9Read8(addr);
    qDebug() << "addr" << addr << "=" << QString::number(value, 16) << "\n";

    return value;
}

int JSBreakPointManager::readRegister(const int regIndex)
{
    const auto value = emuThread->readRegister(regIndex);
    qDebug() << "r" << regIndex << "=" << QString::number(value, 16) << "\n";
    return value;
}

QString JSBreakPointManager::readString(int addr, const QString& encoding)
{
    emuThread->emuPause();
    QTextCodec* codec = QTextCodec::codecForName(encoding.toUtf8());
    QByteArray shiftJISchar;

    while (const uint32_t val = emuThread->NDS->ARM9Read8(addr)) {
        shiftJISchar.push_back(static_cast<char>(val));
        ++addr;
    }
    QString output = codec->toUnicode(shiftJISchar);
    emuThread->emuUnpause();

    return output;

}

void JSBreakPointManager::copyToClipboard(const QString &string)
{
    QClipboard* clipboard = QApplication::clipboard();
    QString trimmed = string.trimmed().remove("\n");
    qDebug() << "copy to clipboard" << trimmed;
    clipboard->setText(trimmed, QClipboard::Clipboard);
    if (clipboard->supportsSelection()) {
        clipboard->setText(trimmed, QClipboard::Selection);
    }
#if defined(Q_OS_LINUX)
    QThread::msleep(1); //workaround for copied text not being available...
#endif
}

void JSBreakPointManager::reset()
{
    for (const int x : breakPointCallbacks.keys()) {
        emuThread->removeBreakPoint(x);
    }
    updateCallbacks.clear();
    breakPointCallbacks.clear();

    jsEngine->collectGarbage();
    jsEngine->globalObject().setProperty("bpManager", jsEngine->newQObject(this));
}

void JSBreakPointManager::registerUpdateFunction(QJSValue updateCallback) {
    updateCallbacks.push_back(updateCallback);
}

void JSBreakPointManager::onTimerUpdate() {
    for (auto& x : updateCallbacks) {
        auto result = x.call(QJSValueList{});

        if (result.isError()) {
            qDebug() 
                << "Uncaught exception at line"
                << result.property("lineNumber").toInt()
                << ":" << result.toString();
        }
    }
}

QString JSBreakPointManager::decodeHex(const QString &inString, const QString &encoding) {
    QByteArray byteArray = QByteArray::fromHex(inString.toUtf8());
    QTextCodec* codec = QTextCodec::codecForName(encoding.toUtf8());
    return codec->toUnicode(byteArray);
}

QString JSBreakPointManager::getLoadedScriptName() const {
    if (loadedFile.isEmpty()) {
        return QString{};
    }

    QFileInfo info(loadedFile);
    return info.fileName();
}
