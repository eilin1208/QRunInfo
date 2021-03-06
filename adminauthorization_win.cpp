/**************************************************************************
**
** Copyright (C) 2015 The Qt Company Ltd.
** Contact: http://www.qt.io/licensing/
**
** This file is part of the Qt Installer Framework.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see http://qt.io/terms-conditions. For further
** information use the contact form at http://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 or version 3 as published by the Free
** Software Foundation and appearing in the file LICENSE.LGPLv21 and
** LICENSE.LGPLv3 included in the packaging of this file. Please review the
** following information to ensure the GNU Lesser General Public License
** requirements will be met: https://www.gnu.org/licenses/lgpl.html and
** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** As a special exception, The Qt Company gives you certain additional
** rights. These rights are described in The Qt Company LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
**
** $QT_END_LICENSE$
**
**************************************************************************/

#ifdef _MSC_VER
#pragma execution_character_set("utf-8")
#endif


#include "adminauthorization.h"

#include <QDebug>
#include <QDir>
#include <QSettings>

#include <qt_windows.h>
#include "win32path.h"

#ifdef Q_CC_MINGW
# ifndef SEE_MASK_NOASYNC
#  define SEE_MASK_NOASYNC 0x00000100
# endif
#endif

QRUNINFO_NAMEPASE_BEGIN
// taken from qprocess_win.cpp
static QString qt_create_commandline(const QString &program, const QStringList &arguments)
{
    QString args;
    if (!program.isEmpty()) {
        QString programName = program;
        if (!programName.startsWith(QLatin1Char('\"')) && !programName.endsWith(QLatin1Char('\"'))
            && programName.contains(QLatin1Char(' '))) {
                programName = QLatin1Char('\"') + programName + QLatin1Char('\"');
        }
        programName.replace(QLatin1Char('/'), QLatin1Char('\\'));

        // add the program as the first arg ... it works better
        args = programName + QLatin1Char(' ');
    }

    for (int i = 0; i < arguments.size(); ++i) {
        QString tmp = arguments.at(i);
        // in the case of \" already being in the string the \ must also be escaped
        tmp.replace(QLatin1String("\\\""), QLatin1String("\\\\\""));
        // escape a single " because the arguments will be parsed
        tmp.replace(QLatin1Char('\"'), QLatin1String("\\\""));
        if (tmp.isEmpty() || tmp.contains(QLatin1Char(' ')) || tmp.contains(QLatin1Char('\t'))) {
            // The argument must not end with a \ since this would be interpreted
            // as escaping the quote -- rather put the \ behind the quote: e.g.
            // rather use "foo"\ than "foo\"
            QString endQuote(QLatin1Char('\"'));
            int i = tmp.length();
            while (i > 0 && tmp.at(i - 1) == QLatin1Char('\\')) {
                --i;
                endQuote += QLatin1Char('\\');
            }
            args += QLatin1String(" \"") + tmp.left(i) + endQuote;
        } else {
            args += QLatin1Char(' ') + tmp;
        }
    }
    return args;
}

bool AdminAuthorization::hasAdminRights()
{
    SID_IDENTIFIER_AUTHORITY authority = { SECURITY_NT_AUTHORITY };
    PSID adminGroup;
    // Initialize SID.
    if (!AllocateAndInitializeSid(&authority,
                                  2,
                                  SECURITY_BUILTIN_DOMAIN_RID,
                                  DOMAIN_ALIAS_RID_ADMINS,
                                  0, 0, 0, 0, 0, 0,
                                  &adminGroup))
        return false;

    BOOL isInAdminGroup = FALSE;
    if (!CheckTokenMembership(0, adminGroup, &isInAdminGroup))
        isInAdminGroup = FALSE;

    FreeSid(adminGroup);
    return isInAdminGroup;
}

//copied from qsystemerror.cpp in Qt
static QString windowsErrorString(int errorCode)
{
    QString ret;
    wchar_t *string = 0;
    FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER|FORMAT_MESSAGE_FROM_SYSTEM,
                  NULL,
                  errorCode,
                  MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                  (LPWSTR)&string,
                  0,
                  NULL);
    ret = QString::fromWCharArray(string);
    LocalFree((HLOCAL)string);

    if (ret.isEmpty() && errorCode == ERROR_MOD_NOT_FOUND)
        ret = QString::fromLatin1("The specified module could not be found.");

    ret.append(QLatin1String(" (0x"));
    ret.append(QString::number(uint(errorCode), 16).rightJustified(8, QLatin1Char('0')));
    ret.append(QLatin1String(")"));

    return ret;
}


bool AdminAuthorization::execute(QWidget *, const QString &program, const QStringList &arguments)
{
    DeCoInitializer _;

    // AdminAuthorization::execute uses UAC to ask for admin privileges. If the user is no
    // administrator yet and the computer's policies are set to not use UAC (which is the case
    // in some corporate networks), the call to execute() will simply succeed and not at all
    // launch the child process. To avoid this, we detect this situation here and return early.
    if (!hasAdminRights()) {
        QLatin1String key("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\"
            "Policies\\System");
        QSettings registry(key, QSettings::NativeFormat);
        const QVariant enableLUA = registry.value(QLatin1String("EnableLUA"));
        if ((enableLUA.type() == QVariant::Int) && (enableLUA.toInt() == 0))
            return false;
    }

    const QString file = QDir::toNativeSeparators(program);
    const QString args = qt_create_commandline(QString(), arguments);

    SHELLEXECUTEINFOW shellExecuteInfo = { 0 };
    shellExecuteInfo.nShow = SW_HIDE;
    shellExecuteInfo.lpVerb = L"runas";
    shellExecuteInfo.lpFile = (wchar_t *)file.utf16();
    shellExecuteInfo.cbSize = sizeof(SHELLEXECUTEINFOW);
    shellExecuteInfo.lpParameters = (wchar_t *)args.utf16();
    shellExecuteInfo.fMask = SEE_MASK_NOASYNC;

    qDebug() << QString::fromLatin1("Starting elevated process %1 with arguments: %2.").arg(file, args);

    if (ShellExecuteExW(&shellExecuteInfo)) {
        qDebug() << "Finished starting elevated process.";
        return true;
    } else {
        qWarning() << QString::fromLatin1("Error while starting elevated process: %1, "
            "Error: %2").arg(program, windowsErrorString(GetLastError()));
    }
    return false;
}

QRUNINFO_NAMEPASE_END
