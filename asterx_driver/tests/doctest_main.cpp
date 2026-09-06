#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <QCoreApplication>

// The session tests drive a QTcpServer through the event loop, so the test
// binary owns a QCoreApplication for its whole lifetime.
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    doctest::Context context;
    context.applyCommandLine(argc, argv);
    return context.run();
}
