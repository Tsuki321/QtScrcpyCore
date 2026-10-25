#pragma once
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QMetaEnum>
#include <cmath>

// Shared by the editor and runtime so a profile cannot be partially applied.
class KeyMapValidation
{
public:
    static int bindingId(const QString &name, bool keyboardOnly = false)
    {
        bool ok = false;
        const auto bytes = name.toLatin1();
        const int key = QMetaEnum::fromType<Qt::Key>().keyToValue(bytes.constData(), &ok);
        if (ok && key != Qt::Key_unknown && key > 0) { return key; }
        if (keyboardOnly) { return 0; }
        const int button = QMetaEnum::fromType<Qt::MouseButtons>().keyToValue(bytes.constData(), &ok);
        // Reject flags such as AllButtons, and give mouse buttons their own namespace.
        return ok && button > 0 && (button & (button - 1)) == 0 ? -button : 0;
    }

    static bool parse(const QByteArray &json, QJsonObject *root, QString *error = nullptr)
    {
        QJsonParseError parseError;
        const auto doc = QJsonDocument::fromJson(json, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            if (error) { *error = QString("Invalid keymap JSON: %1").arg(parseError.errorString()); }
            return false;
        }
        const auto candidate = doc.object();
        const QString problem = validate(candidate);
        if (!problem.isEmpty()) {
            if (error) { *error = problem; }
            return false;
        }
        if (root) { *root = candidate; }
        if (error) { error->clear(); }
        return true;
    }

    static QString validate(const QJsonObject &root)
    {
        QMap<int, QString> bindings;
        const auto bind = [&bindings](const QJsonObject &node, const QString &field,
                                     const QString &path, bool keyboardOnly) -> QString {
            const auto name = node.value(field).toString();
            const int id = bindingId(name, keyboardOnly);
            const auto location = path + "." + field;
            if (!id) { return location + ": choose a valid " + (keyboardOnly ? "keyboard key." : "key or mouse button."); }
            if (bindings.contains(id)) {
                return location + ": " + name + " is already assigned to " + bindings.value(id) + ".";
            }
            bindings.insert(id, location);
            return {};
        };
        QString error = bind(root, "switchKey", "Profile", false);
        if (!error.isEmpty()) { return error; }
        if (root.contains("keyMapNodes") && !root.value("keyMapNodes").isArray()) {
            return "keyMapNodes must be an array.";
        }
        if (root.contains("mouseMoveMap")) {
            if (!root.value("mouseMoveMap").isObject()) { return "mouseMoveMap must be an object."; }
            const auto mouse = root.value("mouseMoveMap").toObject();
            if (!position(mouse, "startPos")) { return "Mouse look: startPos needs x and y between 0 and 1."; }
            const bool legacy = mouse.contains("speedRatio");
            const double base = mouse.value("speedRatio").toDouble(1);
            const double x = mouse.value("speedRatioX").toDouble(base);
            const double y = mouse.value("speedRatioY").toDouble(legacy ? base / 2.25 : 1);
            if ((!legacy && !mouse.contains("speedRatioX") && !mouse.contains("speedRatioY"))
                || !std::isfinite(x) || !std::isfinite(y) || x < .001 || y < .001
                || (mouse.contains("speedRatio") && !number(mouse, "speedRatio", .001, 1000000))
                || (mouse.contains("speedRatioX") && !number(mouse, "speedRatioX", .001, 1000000))
                || (mouse.contains("speedRatioY") && !number(mouse, "speedRatioY", .001, 1000000))) {
                return "Mouse look: supply positive X and Y speed ratios (minimum 0.001), or a legacy speedRatio.";
            }
            if (mouse.contains("smallEyes")) {
                const auto eyes = mouse.value("smallEyes").toObject();
                if (eyes.value("type") != "KMT_CLICK" || !position(eyes, "pos")) {
                    return "Free look (smallEyes) needs type KMT_CLICK and a valid pos.";
                }
                error = bind(eyes, "key", "Free look", false);
                if (!error.isEmpty()) { return error; }
            }
        }
        int steeringCount = 0;
        const auto nodes = root.value("keyMapNodes").toArray();
        for (int i = 0; i < nodes.size(); ++i) {
            const auto node = nodes[i].toObject();
            const auto type = node.value("type").toString();
            const auto path = QString("Mapping %1 (%2)").arg(i + 1).arg(type);
            if (type == "KMT_STEER_WHEEL") {
                if (++steeringCount > 1) { return "Only one steering pad is supported per profile."; }
                if (!position(node, "centerPos")) { return path + ": invalid centerPos."; }
                const auto center = node.value("centerPos").toObject();
                for (const auto &direction : {"left", "right", "up", "down"}) {
                    const QString dir = QString::fromLatin1(direction);
                    error = bind(node, dir + "Key", path, true);
                    if (!error.isEmpty()) { return error; }
                    const double axis = center.value((dir == "left" || dir == "right") ? "x" : "y").toDouble();
                    const double limit = (dir == "left" || dir == "up") ? axis : 1 - axis;
                    if (!number(node, dir + "Offset", 0, limit + 1e-9)) {
                        return path + ": " + dir + "Offset must keep the pad on screen.";
                    }
                }
                continue;
            }
            if (type != "KMT_CLICK" && type != "KMT_CLICK_TWICE" && type != "KMT_CLICK_MULTI"
                && type != "KMT_DRAG" && type != "KMT_ANDROID_KEY") {
                return path + ": unsupported mapping type.";
            }
            error = bind(node, "key", path, false);
            if (!error.isEmpty()) { return error; }
            if ((type == "KMT_CLICK" || type == "KMT_CLICK_TWICE") && !position(node, "pos")) {
                return path + ": pos needs x and y between 0 and 1.";
            }
            if (node.contains("switchMap") && !node.value("switchMap").isBool()) {
                return path + ": switchMap must be true or false.";
            }
            if ((type == "KMT_ANDROID_KEY" || node.contains("androidKey"))
                && !number(node, "androidKey", 0, 65535, true)) {
                return path + ": androidKey must be a nonnegative integer (0-65535).";
            }
            if (type == "KMT_DRAG") {
                if (!position(node, "startPos") || !position(node, "endPos")) { return path + ": invalid swipe endpoints."; }
                if (node.contains("startDelay") && !number(node, "startDelay", 0, 60000, true)) { return path + ": startDelay must be 0-60000 ms."; }
                if (node.contains("dragSpeed") && !number(node, "dragSpeed", 0, 1)) { return path + ": dragSpeed must be 0-1."; }
            }
            if (type == "KMT_CLICK_MULTI") {
                const auto steps = node.value("clickNodes").toArray();
                if (steps.isEmpty() || steps.size() > 50) { return path + ": a sequence needs 1-50 steps."; }
                for (const auto &step : steps) {
                    if (!position(step.toObject(), "pos") || !number(step.toObject(), "delay", 0, 60000, true)) {
                        return path + ": each step needs a position and a delay of 0-60000 ms.";
                    }
                }
            }
        }
        return {};
    }

private:
    static bool number(const QJsonObject &node, const QString &field, double min, double max, bool integer = false)
    {
        const auto value = node.value(field);
        const double n = value.toDouble();
        return value.isDouble() && std::isfinite(n) && n >= min && n <= max && (!integer || std::floor(n) == n);
    }
    static bool position(const QJsonObject &node, const QString &field)
    {
        const auto pos = node.value(field).toObject();
        return number(pos, "x", 0, 1) && number(pos, "y", 0, 1);
    }
};
