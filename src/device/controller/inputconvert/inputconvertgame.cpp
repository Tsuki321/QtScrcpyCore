#include <QDebug>
#include <QCursor>
#include <QGuiApplication>
#include <QTimer>
#include <QTime>
#include <QtMath>
#include <limits>
#include "KeyMapValidation.h"

#ifdef Q_OS_MACOS
#include <CoreGraphics/CoreGraphics.h>
#endif

#include "inputconvertgame.h"

#define CURSOR_POS_CHECK 50

InputConvertGame::InputConvertGame(Controller *controller) : InputConvertNormal(controller) {
    m_ctrlSteerWheel.delayData.timer = new QTimer(this);
    m_ctrlSteerWheel.delayData.timer->setSingleShot(true);
    m_ctrlSteerWheel.delayData.timer->setTimerType(Qt::PreciseTimer);
    connect(m_ctrlSteerWheel.delayData.timer, &QTimer::timeout, this, &InputConvertGame::onSteerWheelTimer);
}

InputConvertGame::~InputConvertGame()
{
    releaseGameInputs();
    hideMouseCursor(false);
    emit grabCursor(false);
}

void InputConvertGame::releaseGameInputs()
{
    releaseNormalInputs();
    m_pressedBindings.clear();
    ++m_inputGeneration;
    m_sequenceGeneration.clear();
    stopMouseMoveTimer();
    m_ctrlSteerWheel.delayData.timer->stop();
    m_ctrlSteerWheel.delayData.queuePos.clear();
    m_ctrlSteerWheel.delayData.queueTimer.clear();
    m_ctrlSteerWheel.pressedUp = m_ctrlSteerWheel.pressedDown = false;
    m_ctrlSteerWheel.pressedLeft = m_ctrlSteerWheel.pressedRight = false;
    m_ctrlSteerWheel.touchKey = Qt::Key_unknown;
    m_ctrlSteerWheel.delayData.pressedNum = 0;
    delete m_dragDelayData.timer;
    m_dragDelayData.timer = nullptr;
    m_dragDelayData.queuePos.clear();
    m_dragDelayData.queueTimer.clear();
    m_dragDelayData.pressKey = 0;
    const auto touches = m_activeTouches;
    for (auto it = touches.cbegin(); it != touches.cend(); ++it) { sendTouchUpEvent(it.key(), it.value()); }
    for (auto &id : m_multiTouchID) { id = 0; }
    const auto keys = m_androidKeys.keys();
    for (int key : keys) { m_androidKeys[key] = 1; sendKeyEvent(AKEY_EVENT_ACTION_UP, static_cast<AndroidKeycode>(key)); }
    m_ctrlMouseMove.touching = false;
    m_ctrlMouseMove.smallEyes = false;
    m_ctrlMouseMove.hasLastPos = false;
    m_ctrlMouseMove.warpPending = false;
}

void InputConvertGame::mouseEvent(const QMouseEvent *from, const QSize &frameSize, const QSize &showSize)
{
    if (!from) { return; }
    // 处理开关按键
    if (m_keyMap.isSwitchOnKeyboard() == false && m_keyMap.getSwitchKey() == static_cast<int>(from->button())) {
        if (from->type() != QEvent::MouseButtonPress) {
            return;
        }
        if (!switchGameMap()) {
            m_needBackMouseMove = false;
        }
        return;
    }

    const auto &mouseNode = m_keyMap.getKeyMapNodeMouse(from->button());
    if (m_gameMap && m_needBackMouseMove && mouseNode.type == KeyMap::KMT_CLICK && mouseNode.data.click.switchMap) {
        updateSize(frameSize, showSize);
        if (processMouseClick(from)) { return; }
    }
    if (!m_needBackMouseMove && m_gameMap) {
        updateSize(frameSize, showSize);
        // mouse move
        if (m_keyMap.isValidMouseMoveMap()) {
            if (processMouseMove(from)) {
                return;
            }
        }
        // mouse click
        if (processMouseClick(from)) {
            return;
        }
    }
    InputConvertNormal::mouseEvent(from, frameSize, showSize);
}

void InputConvertGame::wheelEvent(const QWheelEvent *from, const QSize &frameSize, const QSize &showSize)
{
    if (m_gameMap && !m_needBackMouseMove) {
        updateSize(frameSize, showSize);
    } else {
        InputConvertNormal::wheelEvent(from, frameSize, showSize);
    }
}

void InputConvertGame::keyEvent(const QKeyEvent *from, const QSize &frameSize, const QSize &showSize)
{
    if (!from) { return; }
    if (m_keyMap.isSwitchOnKeyboard() && m_keyMap.getSwitchKey() == from->key()) {
        if (!from->isAutoRepeat() && from->type() == QEvent::KeyPress) { switchGameMap(); }
        return;
    }
    updateSize(frameSize, showSize);
    const auto &node = m_keyMap.getKeyMapNodeKey(from->key());
    if (m_gameMap && (!m_needBackMouseMove || (node.type == KeyMap::KMT_CLICK && node.data.click.switchMap))) {
        if (from->isAutoRepeat()) { return; }
        if (!processFreeLook(KeyMap::AT_KEY, from->key(), from->type() == QEvent::KeyPress)) {
            processMappedKey(node, from);
        }
    } else {
        InputConvertNormal::keyEvent(from, frameSize, showSize);
    }
}

bool InputConvertGame::processFreeLook(KeyMap::ActionType type, int key, bool down)
{
    if (!m_keyMap.isValidMouseMoveMap()) { return false; }
    const auto &eyes = m_keyMap.getMouseMoveMap().data.mouseMove.smallEyes;
    if (eyes.type != type || eyes.key != key) { return false; }
    if (m_ctrlMouseMove.smallEyes == down) { return true; }
    stopMouseMoveTimer();
    mouseMoveStopTouch();
    m_ctrlMouseMove.smallEyes = down;
    mouseMoveStartTouch(nullptr);
    if (!down) { startMouseMoveTimer(); }
    return true;
}

void InputConvertGame::processMappedKey(const KeyMap::KeyMapNode &node, const QKeyEvent *from)
{
    if (from->type() == QEvent::KeyPress) {
        if (m_pressedBindings.contains(from->key())) { return; }
        m_pressedBindings.insert(from->key());
    } else if (from->type() != QEvent::KeyRelease || !m_pressedBindings.remove(from->key())) { return; }
    switch (node.type) {
    case KeyMap::KMT_STEER_WHEEL:
        processSteerWheel(node, from); break;
    case KeyMap::KMT_CLICK:
        processKeyClick(node.data.click.keyNode.pos, false, from);
        processAndroidKey(node.data.click.keyNode.androidKey, from);
        if (node.data.click.switchMap && from->type() == QEvent::KeyRelease && m_keyMap.isValidMouseMoveMap()) {
            // Changing dispatch routes must release held keys, fingers and timers.
            releaseGameInputs();
            m_needBackMouseMove = !m_needBackMouseMove;
            hideMouseCursor(!m_needBackMouseMove);
            emit grabCursor(!m_needBackMouseMove);
        }
        break;
    case KeyMap::KMT_CLICK_TWICE:
        processKeyClick(node.data.clickTwice.keyNode.pos, true, from);
        processAndroidKey(node.data.clickTwice.keyNode.androidKey, from); break;
    case KeyMap::KMT_CLICK_MULTI:
        processKeyClickMulti(node.data.clickMulti.keyNode.delayClickNodes, node.data.clickMulti.keyNode.delayClickNodesCount, from); break;
    case KeyMap::KMT_DRAG:
        processKeyDrag(node.data.drag.keyNode.pos, node.data.drag.keyNode.extendPos,
                       node.data.drag.startDelay, node.data.drag.dragSpeed, from); break;
    case KeyMap::KMT_ANDROID_KEY:
        processAndroidKey(node.data.androidKey.keyNode.androidKey, from); break;
    default: break;
    }
}

bool InputConvertGame::isCurrentCustomKeymap()
{
    return m_gameMap;
}

void InputConvertGame::loadKeyMap(const QString &json)
{
    if (!KeyMapValidation::parse(json.toUtf8(), nullptr)) { return; }
    releaseGameInputs();
    hideMouseCursor(false);
    emit grabCursor(false);
    m_gameMap = false;
    m_needBackMouseMove = false;
    m_keyMap.loadKeyMap(json);
}

void InputConvertGame::updateSize(const QSize &frameSize, const QSize &showSize)
{
    if (frameSize != m_frameSize && !m_frameSize.isEmpty()) { releaseGameInputs(); }
    if (showSize != m_showSize) {
        m_ctrlMouseMove.hasLastPos = false;
        if (m_gameMap && !m_needBackMouseMove && m_keyMap.isValidMouseMoveMap()) {
            // show size change, resize grab cursor
            emit grabCursor(true);
        }
    }
    m_frameSize = frameSize;
    m_showSize = showSize;
}

void InputConvertGame::sendTouchDownEvent(int id, QPointF pos)
{
    sendTouchEvent(id, pos, AMOTION_EVENT_ACTION_DOWN);
}

void InputConvertGame::sendTouchMoveEvent(int id, QPointF pos)
{
    sendTouchEvent(id, pos, AMOTION_EVENT_ACTION_MOVE);
}

void InputConvertGame::sendTouchUpEvent(int id, QPointF pos)
{
    sendTouchEvent(id, pos, AMOTION_EVENT_ACTION_UP);
}

void InputConvertGame::sendTouchEvent(int id, QPointF pos, AndroidMotioneventAction action)
{
    if (0 > id || MULTI_TOUCH_MAX_NUM - 1 < id) {
        return;
    }
    if (m_frameSize.isEmpty() || (action != AMOTION_EVENT_ACTION_DOWN && !m_activeTouches.contains(id))) { return; }
    if (action == AMOTION_EVENT_ACTION_DOWN && m_activeTouches.contains(id)) { return; }
    ControlMsg *controlMsg = new ControlMsg(ControlMsg::CMT_INJECT_TOUCH);
    if (!controlMsg) {
        return;
    }

    QPoint absolutePos = calcFrameAbsolutePos(pos).toPoint();
    if (AMOTION_EVENT_ACTION_MOVE == action && m_activeTouches.contains(id)
            && calcFrameAbsolutePos(m_activeTouches.value(id)).toPoint() == absolutePos) {
        delete controlMsg;
        return;
    }
    m_lastAbsolutePos = absolutePos;
    if (action == AMOTION_EVENT_ACTION_UP) { m_activeTouches.remove(id); }
    else { m_activeTouches[id] = pos; }

    controlMsg->setInjectTouchMsgData(
        static_cast<quint64>(id),
        action,
        static_cast<AndroidMotioneventButtons>(0),
        static_cast<AndroidMotioneventButtons>(0),
        QRect(absolutePos, m_frameSize),
        AMOTION_EVENT_ACTION_UP == action ? 0.0f : 1.0f);
    sendControlMsg(controlMsg);
}

void InputConvertGame::sendKeyEvent(AndroidKeyeventAction action, AndroidKeycode keyCode) {
    ControlMsg *controlMsg = new ControlMsg(ControlMsg::CMT_INJECT_KEYCODE);
    if (!controlMsg) {
        return;
    }

    const int code = static_cast<int>(keyCode);
    if (action == AKEY_EVENT_ACTION_DOWN) {
        if (++m_androidKeys[code] > 1) { delete controlMsg; return; }
    } else {
        if (!m_androidKeys.contains(code) || --m_androidKeys[code] > 0) { delete controlMsg; return; }
        m_androidKeys.remove(code);
    }
    controlMsg->setInjectKeycodeMsgData(action, keyCode, 0, AMETA_NONE);
    sendControlMsg(controlMsg);
}

QPointF InputConvertGame::calcFrameAbsolutePos(QPointF relativePos)
{
    QPointF absolutePos;
    absolutePos.setX(qBound(0.0, m_frameSize.width() * relativePos.x(), double(qMax(0, m_frameSize.width() - 1))));
    absolutePos.setY(qBound(0.0, m_frameSize.height() * relativePos.y(), double(qMax(0, m_frameSize.height() - 1))));
    return absolutePos;
}

QPointF InputConvertGame::calcScreenAbsolutePos(QPointF relativePos)
{
    QPointF absolutePos;
    absolutePos.setX(m_showSize.width() * relativePos.x());
    absolutePos.setY(m_showSize.height() * relativePos.y());
    return absolutePos;
}

int InputConvertGame::attachTouchID(int key)
{
    const int existing = getTouchID(key);
    if (existing >= 0) { return existing; }
    for (int i = 0; i < MULTI_TOUCH_MAX_NUM; i++) {
        if (0 == m_multiTouchID[i]) {
            m_multiTouchID[i] = key;
            return i;
        }
    }
    return -1;
}

void InputConvertGame::detachTouchID(int key)
{
    for (int i = 0; i < MULTI_TOUCH_MAX_NUM; i++) {
        if (key == m_multiTouchID[i]) {
            m_multiTouchID[i] = 0;
            return;
        }
    }
}

int InputConvertGame::getTouchID(int key)
{
    for (int i = 0; i < MULTI_TOUCH_MAX_NUM; i++) {
        if (key == m_multiTouchID[i]) {
            return i;
        }
    }
    return -1;
}

// -------- steer wheel event --------

void InputConvertGame::getDelayQueue(const QPointF& start, const QPointF& end,
                                     const double& distanceStep, const double& posStepconst,
                                     quint32 lowestTimer, quint32 highestTimer,
                                     QQueue<QPointF>& queuePos, QQueue<quint32>& queueTimer) {
    Q_UNUSED(posStepconst);
    queuePos.clear();
    queueTimer.clear();
    const QPointF delta = end - start;
    const int steps = qMax(1, int(std::ceil(qMax(std::abs(delta.x()), std::abs(delta.y())) / distanceStep)));
    const quint32 interval = qMax(quint32(1), (lowestTimer + highestTimer) / 2);
    for (int i = 1; i <= steps; ++i) {
        queuePos.enqueue(start + delta * (double(i) / steps));
        queueTimer.enqueue(interval);
    }
}

void InputConvertGame::onSteerWheelTimer() {
    if(m_ctrlSteerWheel.delayData.queuePos.empty()) {
        return;
    }
    int id = getTouchID(m_ctrlSteerWheel.touchKey);
    m_ctrlSteerWheel.delayData.currentPos = m_ctrlSteerWheel.delayData.queuePos.dequeue();
    sendTouchMoveEvent(id, m_ctrlSteerWheel.delayData.currentPos);

    if(m_ctrlSteerWheel.delayData.queuePos.empty() && m_ctrlSteerWheel.delayData.pressedNum == 0) {
        sendTouchUpEvent(id, m_ctrlSteerWheel.delayData.currentPos);
        detachTouchID(m_ctrlSteerWheel.touchKey);
        return;
    }

    if(!m_ctrlSteerWheel.delayData.queuePos.empty()) {
        m_ctrlSteerWheel.delayData.timer->start(m_ctrlSteerWheel.delayData.queueTimer.dequeue());
    }
}

void InputConvertGame::processSteerWheel(const KeyMap::KeyMapNode &node, const QKeyEvent *from)
{
    int key = from->key();
    bool flag = from->type() == QEvent::KeyPress;
    // identify keys
    if (key == node.data.steerWheel.up.key) {
        m_ctrlSteerWheel.pressedUp = flag;
    } else if (key == node.data.steerWheel.right.key) {
        m_ctrlSteerWheel.pressedRight = flag;
    } else if (key == node.data.steerWheel.down.key) {
        m_ctrlSteerWheel.pressedDown = flag;
    } else { // left
        m_ctrlSteerWheel.pressedLeft = flag;
    }

    // calc offset and pressed number
    QPointF offset(0.0, 0.0);
    int pressedNum = 0;
    if (m_ctrlSteerWheel.pressedUp) {
        ++pressedNum;
        offset.ry() -= node.data.steerWheel.up.extendOffset;
    }
    if (m_ctrlSteerWheel.pressedRight) {
        ++pressedNum;
        offset.rx() += node.data.steerWheel.right.extendOffset;
    }
    if (m_ctrlSteerWheel.pressedDown) {
        ++pressedNum;
        offset.ry() += node.data.steerWheel.down.extendOffset;
    }
    if (m_ctrlSteerWheel.pressedLeft) {
        ++pressedNum;
        offset.rx() -= node.data.steerWheel.left.extendOffset;
    }
    m_ctrlSteerWheel.delayData.pressedNum = pressedNum;

    // last key release and timer no active, active timer to detouch
    if (pressedNum == 0) {
        if (m_ctrlSteerWheel.delayData.timer->isActive()) {
            m_ctrlSteerWheel.delayData.timer->stop();
            m_ctrlSteerWheel.delayData.queueTimer.clear();
            m_ctrlSteerWheel.delayData.queuePos.clear();
        }

        sendTouchUpEvent(getTouchID(m_ctrlSteerWheel.touchKey), m_ctrlSteerWheel.delayData.currentPos);
        detachTouchID(m_ctrlSteerWheel.touchKey);
        return;
    }

    // process steer wheel key event
    m_ctrlSteerWheel.delayData.timer->stop();
    m_ctrlSteerWheel.delayData.queueTimer.clear();
    m_ctrlSteerWheel.delayData.queuePos.clear();

    // first press, get key and touch down
    if (pressedNum == 1 && flag) {
        m_ctrlSteerWheel.touchKey = from->key();
        int id = attachTouchID(m_ctrlSteerWheel.touchKey);
        sendTouchDownEvent(id, node.data.steerWheel.centerPos);
        m_ctrlSteerWheel.delayData.currentPos = node.data.steerWheel.centerPos;

        getDelayQueue(node.data.steerWheel.centerPos, node.data.steerWheel.centerPos+offset,
                      0.01f, 0.002f, 2, 8,
                      m_ctrlSteerWheel.delayData.queuePos,
                      m_ctrlSteerWheel.delayData.queueTimer);
    } else {
        getDelayQueue(m_ctrlSteerWheel.delayData.currentPos, node.data.steerWheel.centerPos+offset,
                      0.01f, 0.002f, 2, 8,
                      m_ctrlSteerWheel.delayData.queuePos,
                      m_ctrlSteerWheel.delayData.queueTimer);
    }
    m_ctrlSteerWheel.delayData.timer->start(m_ctrlSteerWheel.delayData.queueTimer.dequeue());
    return;
}

// -------- key event --------

void InputConvertGame::processKeyClick(const QPointF &clickPos, bool clickTwice, const QKeyEvent *from)
{
    if (QEvent::KeyPress == from->type()) {
        int id = attachTouchID(from->key());
        sendTouchDownEvent(id, clickPos);
        if (clickTwice) {
            sendTouchUpEvent(getTouchID(from->key()), clickPos);
            detachTouchID(from->key());
        }
    } else if (QEvent::KeyRelease == from->type()) {
        if (clickTwice) {
            int id = attachTouchID(from->key());
            sendTouchDownEvent(id, clickPos);
        }
        sendTouchUpEvent(getTouchID(from->key()), clickPos);
        detachTouchID(from->key());
    }
}

void InputConvertGame::processKeyClickMulti(const KeyMap::DelayClickNode *nodes, const int count, const QKeyEvent *from)
{
    if (QEvent::KeyPress != from->type()) {
        return;
    }

    int key = from->key();
    const quint64 generation = m_inputGeneration;
    const quint64 sequence = ++m_sequenceGeneration[key];
    const int previous = getTouchID(key);
    if (previous >= 0) { sendTouchUpEvent(previous, m_activeTouches.value(previous)); detachTouchID(key); }
    int delay = 0;
    QPointF clickPos;

    for (int i = 0; i < count; i++) {
        delay += nodes[i].delay;
        clickPos = nodes[i].pos;
        QTimer::singleShot(delay, this, [this, key, clickPos, generation, sequence]() {
            if (generation != m_inputGeneration || sequence != m_sequenceGeneration.value(key)) { return; }
            int id = attachTouchID(key);
            sendTouchDownEvent(id, clickPos);
        });

        // Don't up it too fast
        delay += 20;
        QTimer::singleShot(delay, this, [this, key, clickPos, generation, sequence]() {
            if (generation != m_inputGeneration || sequence != m_sequenceGeneration.value(key)) { return; }
            int id = getTouchID(key);
            sendTouchUpEvent(id, clickPos);
            detachTouchID(key);
        });
    }
}

void InputConvertGame::onDragTimer() {
    if(m_dragDelayData.queuePos.empty()) {
        return;
    }
    int id = getTouchID(m_dragDelayData.pressKey);
    m_dragDelayData.currentPos = m_dragDelayData.queuePos.dequeue();
    sendTouchMoveEvent(id, m_dragDelayData.currentPos);

    if(m_dragDelayData.queuePos.empty()) {
        delete m_dragDelayData.timer;
        m_dragDelayData.timer = nullptr;

        return;
    }

    if(!m_dragDelayData.queuePos.empty()) {
        m_dragDelayData.timer->start(m_dragDelayData.queueTimer.dequeue());
    }
}

void InputConvertGame::processKeyDrag(const QPointF &startPos, QPointF endPos, quint32 startDelay, float dragSpeed, const QKeyEvent *from)
{
    if (from->type() == QEvent::KeyRelease && from->key() != m_dragDelayData.pressKey) { return; }
    if (from->type() == QEvent::KeyPress || from->type() == QEvent::KeyRelease) {
        // Stop the previous swipe, including a held finger at its endpoint.
        if (m_dragDelayData.pressKey) {
            if (m_dragDelayData.timer) { m_dragDelayData.timer->stop(); }
            delete m_dragDelayData.timer;
            m_dragDelayData.timer = nullptr;
            m_dragDelayData.queuePos.clear();
            m_dragDelayData.queueTimer.clear();

            sendTouchUpEvent(getTouchID(m_dragDelayData.pressKey), m_dragDelayData.currentPos);
            detachTouchID(m_dragDelayData.pressKey);

            m_dragDelayData.currentPos = QPointF();
            m_dragDelayData.pressKey = 0;
        }

        if (from->type() == QEvent::KeyRelease) { return; }
        // start this
        int id = attachTouchID(from->key());
        if (id < 0) { return; }
        sendTouchDownEvent(id, startPos);

        m_dragDelayData.timer = new QTimer(this);
        m_dragDelayData.timer->setSingleShot(true);
        m_dragDelayData.timer->setTimerType(Qt::PreciseTimer);
        connect(m_dragDelayData.timer, &QTimer::timeout, this, &InputConvertGame::onDragTimer);
        m_dragDelayData.pressKey = from->key();
        m_dragDelayData.currentPos = startPos;
        m_dragDelayData.queuePos.clear();
        m_dragDelayData.queueTimer.clear();

        // Clamp dragSpeed to 0-1 range
        const float speed = qBound(0.0f, static_cast<float>(dragSpeed), 1.0f);
        
        // Calculate delays based on dragSpeed
        // dragSpeed = 1 -> minDelay = 1, maxDelay = 2 (fastest)
        // dragSpeed = 0 -> minDelay = 30, maxDelay = 40 (slowest)
        const quint32 minDelay = static_cast<quint32>(1 + (1.0f - speed) * 29);  // 1 to 30
        const quint32 maxDelay = minDelay + static_cast<quint32>((1.0f - speed) * 9) + 1;

        getDelayQueue(startPos, endPos,
                      0.01f, 0.0005f,
                      minDelay,
                      maxDelay,
                      m_dragDelayData.queuePos,
                      m_dragDelayData.queueTimer);

        m_dragDelayData.timer->start(startDelay + m_dragDelayData.queueTimer.dequeue());
    }
}

void InputConvertGame::processAndroidKey(AndroidKeycode androidKey, const QKeyEvent *from)
{
    if (AKEYCODE_UNKNOWN == androidKey) {
        return;
    }

    AndroidKeyeventAction action;
    switch (from->type()) {
    case QEvent::KeyPress:
        action = AKEY_EVENT_ACTION_DOWN;
        break;
    case QEvent::KeyRelease:
        action = AKEY_EVENT_ACTION_UP;
        break;
    default:
        return;
    }

    sendKeyEvent(action, androidKey);
}

// -------- mouse event --------

bool InputConvertGame::processMouseClick(const QMouseEvent *from)
{
    if (from->type() != QEvent::MouseButtonPress && from->type() != QEvent::MouseButtonDblClick
        && from->type() != QEvent::MouseButtonRelease) { return false; }
    const bool down = from->type() != QEvent::MouseButtonRelease;
    if (processFreeLook(KeyMap::AT_MOUSE, int(from->button()), down)) { return true; }
    const auto &node = m_keyMap.getKeyMapNodeMouse(from->button());
    if (node.type == KeyMap::KMT_INVALID) { return false; }
    QKeyEvent mapped(down ? QEvent::KeyPress : QEvent::KeyRelease, -int(from->button()), Qt::NoModifier);
    processMappedKey(node, &mapped);
    return true;
}

bool InputConvertGame::processMouseMove(const QMouseEvent *from)
{
    if (from->type() != QEvent::MouseMove) { return false; }
    if (m_showSize.isEmpty() || m_frameSize.isEmpty()) { return true; }
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    const QPointF pos = from->localPos();
#else
    const QPointF pos = from->position();
#endif
    if (m_ctrlMouseMove.warpPending) {
        m_ctrlMouseMove.warpPending = false;
        // Ignore only the synthetic recenter event. A coalesced real delta still counts.
        m_ctrlMouseMove.lastPos = m_ctrlMouseMove.warpPos;
        if (pos == m_ctrlMouseMove.warpPos) { return true; }
    }
    if (!m_ctrlMouseMove.hasLastPos) {
        m_ctrlMouseMove.lastPos = pos;
        m_ctrlMouseMove.hasLastPos = true;
        checkCursorPos(from);
        return true;
    }
    const QPointF pixels = pos - m_ctrlMouseMove.lastPos;
    m_ctrlMouseMove.lastPos = pos;
    if (!pixels.isNull()) {
        const auto ratio = m_keyMap.getMouseMoveMap().data.mouseMove.speedRatio;
        QPointF remaining(pixels.x() / (ratio.x() * m_showSize.width()),
                          pixels.y() / (ratio.y() * m_showSize.height()));
        mouseMoveStartTouch(from);
        // Split motion at the touch boundary and carry the remainder into the next
        // swipe in this same event. No dropped events or delayed touch restarts.
        for (int segment = 0; segment < 128 && !remaining.isNull() && m_ctrlMouseMove.touching; ++segment) {
            const auto start = m_ctrlMouseMove.lastConverPos;
            double fraction = 1.0;
            if (remaining.x() > 0) { fraction = qMin(fraction, (1 - start.x()) / remaining.x()); }
            if (remaining.x() < 0) { fraction = qMin(fraction, -start.x() / remaining.x()); }
            if (remaining.y() > 0) { fraction = qMin(fraction, (1 - start.y()) / remaining.y()); }
            if (remaining.y() < 0) { fraction = qMin(fraction, -start.y() / remaining.y()); }
            fraction = qBound(0.0, fraction, 1.0);
            m_ctrlMouseMove.lastConverPos = start + remaining * fraction;
            sendTouchMoveEvent(getTouchID(std::numeric_limits<int>::min()), m_ctrlMouseMove.lastConverPos);
            remaining *= 1 - fraction;
            if (fraction < 1) {
                mouseMoveStopTouch();
                mouseMoveStartTouch(from);
                // An origin exactly on the edge cannot move further outwards.
                if (fraction == 0 && m_ctrlMouseMove.lastConverPos == start) { break; }
            }
        }
        startMouseMoveTimer();
    }
    // Consume the movement that reached the desktop boundary before recentering.
    checkCursorPos(from);
    return true;
}

bool InputConvertGame::checkCursorPos(const QMouseEvent *from)
{
    const QPoint pos = from->pos();
    const int margin = qMin(CURSOR_POS_CHECK, qMin(m_showSize.width(), m_showSize.height()) / 4);
    if (margin <= 0 || (pos.x() >= margin && pos.x() <= m_showSize.width() - margin
        && pos.y() >= margin && pos.y() <= m_showSize.height() - margin)) { return false; }
    const QPoint center(m_showSize.width() / 2, m_showSize.height() / 2);
    m_ctrlMouseMove.warpPos = center;
    m_ctrlMouseMove.warpPending = true;
    m_ctrlMouseMove.lastPos = center;
    moveCursorTo(from, center);
    return true;
}

void InputConvertGame::moveCursorTo(const QMouseEvent *from, const QPoint &localPosPixel)
{
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    QPoint posOffset = from->pos() - localPosPixel;
    QPoint globalPos = from->globalPos();
#else
    QPoint posOffset = from->position().toPoint() - localPosPixel;
    QPoint globalPos = from->globalPosition().toPoint();
#endif
    globalPos -= posOffset;
    //qDebug()<<"move cursor to "<<globalPos<<" offset "<<posOffset;
#ifdef Q_OS_MACOS
    // On macOS, QCursor::setPos() posts a synthetic mouse-moved event (CGEventPost)
    // and does NOT re-associate the hardware mouse with the on-screen cursor, so the
    // warp does not "stick": the next hardware delta snaps the cursor back. That makes
    // mouseMoveMap re-centering a no-op, and the camera can no longer pan past the edge.
    // CGWarpMouseCursorPosition performs a real warp, and
    // CGAssociateMouseAndMouseCursorPosition(true) immediately restores the
    // mouse<->cursor association so subsequent hardware deltas keep flowing. This
    // mirrors the approach already used in util/mousetap/cocoamousetap.mm.
    CGWarpMouseCursorPosition(CGPointMake(globalPos.x(), globalPos.y()));
    CGAssociateMouseAndMouseCursorPosition(true);
#else
    QCursor::setPos(globalPos);
#endif
}

void InputConvertGame::mouseMoveStartTouch(const QMouseEvent *from)
{
    Q_UNUSED(from)
    if (!m_ctrlMouseMove.touching) {
        QPointF mouseMoveStartPos
            = m_ctrlMouseMove.smallEyes ? m_keyMap.getMouseMoveMap().data.mouseMove.smallEyes.pos : m_keyMap.getMouseMoveMap().data.mouseMove.startPos;
        int id = attachTouchID(std::numeric_limits<int>::min());
        sendTouchDownEvent(id, mouseMoveStartPos);
        m_ctrlMouseMove.lastConverPos = mouseMoveStartPos;
        m_ctrlMouseMove.touching = id >= 0 && m_activeTouches.contains(id);
    }
}

void InputConvertGame::mouseMoveStopTouch()
{
    if (m_ctrlMouseMove.touching) {
        sendTouchUpEvent(getTouchID(std::numeric_limits<int>::min()), m_ctrlMouseMove.lastConverPos);
        detachTouchID(std::numeric_limits<int>::min());
        m_ctrlMouseMove.touching = false;
    }
}

void InputConvertGame::startMouseMoveTimer()
{
    stopMouseMoveTimer();
    m_ctrlMouseMove.timer = startTimer(500);
}

void InputConvertGame::stopMouseMoveTimer()
{
    if (0 != m_ctrlMouseMove.timer) {
        killTimer(m_ctrlMouseMove.timer);
        m_ctrlMouseMove.timer = 0;
    }
}

bool InputConvertGame::switchGameMap()
{
    releaseGameInputs();
    m_needBackMouseMove = false;
    m_gameMap = !m_gameMap;
    qInfo() << QString("current keymap mode: %1").arg(m_gameMap ? "custom" : "normal");

    if (!m_keyMap.isValidMouseMoveMap()) {
        return m_gameMap;
    }
#ifdef QT_NO_DEBUG
    // grab cursor and set cursor only mouse move map
    emit grabCursor(m_gameMap);
#endif
    hideMouseCursor(m_gameMap);

    if (!m_gameMap) {
        stopMouseMoveTimer();
        mouseMoveStopTouch();
    }

    return m_gameMap;
}

void InputConvertGame::hideMouseCursor(bool hide)
{
    if (m_cursorHidden == hide) { return; }
    m_cursorHidden = hide;
    if (hide) {
#ifdef QT_NO_DEBUG
        QGuiApplication::setOverrideCursor(QCursor(Qt::BlankCursor));
#else
        QGuiApplication::setOverrideCursor(QCursor(Qt::CrossCursor));
#endif
    } else {
        QGuiApplication::restoreOverrideCursor();
    }
}

void InputConvertGame::timerEvent(QTimerEvent *event)
{
    if (m_ctrlMouseMove.timer == event->timerId()) {
        stopMouseMoveTimer();
        // Don't auto-reset view when smallEyes mode is active
        if (!m_ctrlMouseMove.smallEyes) {
            mouseMoveStopTouch();
        }
    }
}
