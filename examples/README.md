# Third-party client examples

## Java JDBC

Build the driver and compile the example:

```bash
./sdk/java/build.sh
mkdir -p sdk/java/build/example
javac -cp sdk/java/build/csudb-jdbc-2026.1.0.jar \
  -d sdk/java/build/example examples/JdbcExample.java
java -cp sdk/java/build/csudb-jdbc-2026.1.0.jar:sdk/java/build/example \
  JdbcExample
```

The JDBC URL is `jdbc:csudb://127.0.0.1:6789/school`. The example reads the
password from the terminal and performs a parameterized, read-only query using
standard `java.sql` interfaces. See `sdk/java/README.md` for supported features.

## Python

Start the Native-protocol server in one terminal:

```bash
csudbd
```

Run the read-only example from the repository root in another terminal:

```bash
python3 examples/python_client.py
```

Enter your CSUDB password at the prompt. By default, this connects to
`127.0.0.1:6789`, database `school`, and queries students aged 20 or older.
The script prints the returned columns, rows, and Buffer Pool summary; it does
not create or modify data.

Choose another connection or filter with `--host`, `--port`, `--user`,
`--database`, and `--min-age`. For example:

```bash
python3 examples/python_client.py --database school --min-age 21
```

The sample imports the driver from this repository's `sdk/python` directory.
An external project can import the installed driver by setting:

```bash
export PYTHONPATH="$HOME/.local/share/csudb/sdk/python:$PYTHONPATH"
```

Then use `import csudb` and `csudb.connect(...)` in that project. The driver
uses the existing Native protocol, so applications do not need to link against
CSUDB's C++ database internals. It is DB-API-style, but it does not yet offer
server-side prepared statements or typed Python result conversion.
