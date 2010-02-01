Gerrit Service for Windows
==========================

This is a daemon for running Gerrit Code Review as a Windows Service.

Building
--------

* Start with a command prompt set up with the Platform SDK build
  environment.  We've only tested with Platform SDK version 6.1.  The
  build may be configured to target XP or later.

* Compile gerrit_service:

  > cl /EHsc gerrit_service.cpp

Installing
----------

The command-line options are as follows:

> Usage: gerrit_service.exe -d GERRIT_SITE [options...]
>    -j <JAVA_HOME>   : Set JAVA_HOME
>    -d <GERRIT_SITE> : Set GERRIT_SITE
>    -g <path>        : Set the path to gerrit.war.
>                       Defaults to $GERRIT_SITE\bin\gerrit.war
>    -i               : Installs the service.  Cannot be used with -u.
>    -u               : Uninstall the service.  Cannot be used with -i.
>
> The following options are used with -i :
>    -S <display name>: Set the display name of the service.
>                       Defaults to "Gerrit Code Review Service".  The name
>                       must be quoted.  Can only be used with -i.
>    -a <account>     : Set the account name under which the service will run.
>                       Defaults to "NT AUTHORITY\LocalSystem".
>    -p <password>    : The password for the account.
>                       Defaults to the empty string.

When initializing the Gerrit working directory, it is recommended that
the `gerrit_service.exe` be copied to `$GERRIT_SITE\bin`.  The process
of installation assumes that the path to `gerrit_service.exe` is the
location of the launched executable.  Therefore the installation
should be conducted from the `$GERRIT_SITE\bin` directory.

For example, assuming that the Gerrit working directory is
`C:\gerrit`, and `gerrit_service.exe` is copied to `C:\gerrit\bin`,
the following command will install and start the Gerrit service:

> gerrit_service.exe -j "C:\Program Files\Java\jre6" -d C:\gerrit -i

If the Gerrit service should run under a different user account, you
can use the `-a` and `-p` options to specify the service user account.

> gerrit_service.exe -j "C:\Program Files\Java\jre6" -d C:\gerrit \
>         -i -a gerrituser -p top-sekkrit

Running
-------

The `-i` option installs and starts the service.  Once installed, the
service is configured to start automatically when the computer starts
and run under either the `Local System` account (by default) or
another configured account.  You may change these settings using the
`Services` configuration applet.

Since it is configured as a standard Windows service, you can stop and
start the daemon using `net stop`, `net start` commands or the
`Services` configuration applet.

Uninstalling
------------

Use:

> gerrit_service.exe -u

to uninstall the service.

