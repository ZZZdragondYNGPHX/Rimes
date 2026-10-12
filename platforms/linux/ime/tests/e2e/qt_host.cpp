#include <QApplication>
#include <QFile>
#include <QLineEdit>
#include <QTextStream>
#include <QTimer>

namespace {

QString g_output_path;

void WriteState(const QString& kind, const QString& text) {
    QFile file(g_output_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return;
    }
    QTextStream stream(&file);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    stream.setCodec("UTF-8");
#endif
    stream << kind << '\t' << text << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        return 1;
    }
    g_output_path = QString::fromLocal8Bit(argv[1]);
    QApplication app(argc, argv);
    QLineEdit entry;
    entry.setWindowTitle(QStringLiteral("RIMES Qt host"));
    entry.resize(480, 80);
    QObject::connect(&entry, &QLineEdit::textChanged, [&](const QString& text) {
        WriteState(QStringLiteral("commit"), text);
    });
    entry.show();
    entry.activateWindow();
    entry.setFocus();
    WriteState(QStringLiteral("ready"), QString());
    if (argc >= 3) {
        const int timeout_ms = QString::fromLocal8Bit(argv[2]).toInt();
        if (timeout_ms > 0) {
            QTimer::singleShot(timeout_ms, &app, &QApplication::quit);
        }
    }
    return app.exec();
}
