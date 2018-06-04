block_access
============

**block_access** is an extension to block access of roles according to date and
time intervals.

When installed, it will take action after a successful authentication. If
current date and time are outside of any `intervals`, access will be block even
if user provides the correct credentials (even superusers are blocked). A list
of roles (`exclude_roles`) that aren't enforced at each interval can be
provided.

It works on different operational systems. It was tested on Linux and FreeBSD.
It compiles on Windows, however, it does not work (it seems auth_delay -- that
uses the same hook -- does not work too).

Installing
----------

Build and install is done using PGXS. PostgreSQL 9.1+ installed (including the
header files). As long as `pg_config` is available in the path, build and
install using:

```
$ make
$ make install
```

Once the library is installed, it needs to be enabled in
`shared_preload_libraries` in postgresql.conf:

```
shared_preload_libraries = 'block_access'
```

If other libraries are already configured for loading, it can be appended to
the end of the list (order does not matter).

Configuration
-------------

By default, `block_access` will not block any connection attempt.

To add an interval, set the value of `block_access.intervals` to a list of
abbreviated week days (sun, mon, tue, wed, thu, fri, sat), dash (-), start time
(08:00), dash (-) and end time (18:00). Example: 'mon, wed, fri - 08:00-18:00'.
It will allow access only on Mondays, Wednesdays and Fridays from 08:00 until
18:00. Any access outside this interval will be blocked (for example, Tuesday
all day or Friday night). If you want to specify more than one interval, use a
semicolon (;) to separate the intervals.

To add a list of exceptions, set the value of `block_access.exclude_roles` to a
list of roles separated by comma (,). If you provide more than on interval, you
should provide the same number of role lists. Use a semicolon (;) to separate
the role lists. Unfortunately, **you cannot provide a role group to block all
of its members** (`block_access` cannot access catalog at this phase). Even
superusers are blocked but any rule.

```
block_access.intervals = 'mon, tue, wed, thu, fri - 08:00-18:00 ; sat - 08:00-12:00'
block_access.exclude_roles = 'postgres, euler ; postgres, bob, alice'
```

It will provide access to databases if date and time are inside those two
rules.  Since role `postgres` is in both list of exceptions, it won't be
blocked.  Role `euler` has access from Monday to Friday any time (because it is
in the list of exceptions), however, it will be blocked on Saturday afternoon
and night.  Since, Sunday is not defined, access is blocked for all roles.

License
-------

> Copyright © 2017-2018 Euler Taveira de Oliveira
> All rights reserved.

> Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

> Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer;
> Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution;
> Neither the name of the Euler Taveira de Oliveira nor the names of its contributors may be used to endorse or promote products derived from this software without specific prior written permission.

> THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

