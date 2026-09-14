import java.io.Console;
import java.sql.Connection;
import java.sql.DriverManager;
import java.sql.PreparedStatement;
import java.sql.ResultSet;
import java.sql.ResultSetMetaData;
import java.util.Properties;

/** Read-only example using CSUDB through standard java.sql interfaces. */
public final class JdbcExample {
  public static void main(String[] args) throws Exception {
    String host = args.length > 0 ? args[0] : "127.0.0.1";
    String database = args.length > 1 ? args[1] : "school";
    int minimumAge = args.length > 2 ? Integer.parseInt(args[2]) : 20;
    String password = readPassword();

    Properties properties = new Properties();
    properties.setProperty("user", "root");
    properties.setProperty("password", password);

    String url = "jdbc:csudb://" + host + ":6789/" + database;
    try (Connection connection = DriverManager.getConnection(url, properties);
         PreparedStatement statement = connection.prepareStatement(
             "SELECT id, name, age, major FROM student WHERE age >= ?;")) {
      System.out.println("Connected: " + connection.getMetaData().getDatabaseProductName()
          + " " + connection.getMetaData().getDatabaseProductVersion());
      System.out.println("URL: " + connection.getMetaData().getURL());

      statement.setInt(1, minimumAge);
      try (ResultSet rows = statement.executeQuery()) {
        ResultSetMetaData columns = rows.getMetaData();
        for (int column = 1; column <= columns.getColumnCount(); column++) {
          if (column > 1) System.out.print(" | ");
          System.out.print(columns.getColumnLabel(column));
        }
        System.out.println();

        int count = 0;
        while (rows.next()) {
          System.out.printf("%d | %s | %d | %s%n",
              rows.getInt("id"), rows.getString("name"),
              rows.getInt("age"), rows.getString("major"));
          count++;
        }
        System.out.println("Rows: " + count);
      }
    }
  }

  private static String readPassword() {
    String environment = System.getenv("CSUDB_PASSWORD");
    if (environment != null && !environment.isEmpty()) return environment;
    Console console = System.console();
    if (console == null) {
      throw new IllegalStateException(
          "interactive console unavailable; set CSUDB_PASSWORD for this process");
    }
    char[] secret = console.readPassword("CSUDB password for root: ");
    return secret == null ? "" : new String(secret);
  }
}
