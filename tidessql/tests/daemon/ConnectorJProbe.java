import java.io.InputStream;
import java.io.OutputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.KeyStore;
import java.security.cert.Certificate;
import java.security.cert.CertificateFactory;
import java.sql.Connection;
import java.sql.DriverManager;
import java.sql.PreparedStatement;
import java.sql.ResultSet;
import java.sql.Statement;
import java.util.Properties;

final class ConnectorJProbe {
  private static final char[] TRUST_PASSWORD = "tidessql-test".toCharArray();

  private static Path createTrustStore(Path certificatePath) throws Exception {
    Certificate certificate;
    try (InputStream input = Files.newInputStream(certificatePath)) {
      certificate = CertificateFactory.getInstance("X.509").generateCertificate(input);
    }
    KeyStore store = KeyStore.getInstance("PKCS12");
    store.load(null, TRUST_PASSWORD);
    store.setCertificateEntry("tidessql-test-root", certificate);
    Path path = Files.createTempFile("tidessql-connector-j-", ".p12");
    try (OutputStream output = Files.newOutputStream(path)) {
      store.store(output, TRUST_PASSWORD);
    }
    return path;
  }

  public static void main(String[] arguments) throws Exception {
    if (arguments.length != 2) throw new IllegalArgumentException("expected port and CA certificate");
    int port = Integer.parseInt(arguments[0]);
    Path trustStore = createTrustStore(Path.of(arguments[1]));
    try {
      Properties properties = new Properties();
      properties.setProperty("user", "alice");
      properties.setProperty("password", "test-password");
      properties.setProperty("sslMode", "VERIFY_IDENTITY");
      properties.setProperty("trustCertificateKeyStoreUrl", trustStore.toUri().toString());
      properties.setProperty("trustCertificateKeyStoreType", "PKCS12");
      properties.setProperty("trustCertificateKeyStorePassword", new String(TRUST_PASSWORD));
      properties.setProperty("connectTimeout", "5000");
      properties.setProperty("socketTimeout", "5000");
      String url = "jdbc:mysql://localhost:" + port + "/tenant";
      try (Connection connection = DriverManager.getConnection(url, properties)) {
        connection.setAutoCommit(false);
        try (Statement statement = connection.createStatement()) {
          statement.executeUpdate(
              "CREATE TABLE connector_j_items(id BIGINT PRIMARY KEY,value BIGINT NOT NULL)");
        }
        try (PreparedStatement statement = connection.prepareStatement(
                 "INSERT INTO connector_j_items VALUES(?,?)")) {
          statement.setLong(1, 1L);
          statement.setLong(2, 42L);
          if (statement.executeUpdate() != 1) throw new IllegalStateException("insert count");
        }
        connection.commit();
        try (PreparedStatement statement = connection.prepareStatement(
                 "SELECT value FROM connector_j_items WHERE id=?")) {
          statement.setLong(1, 1L);
          try (ResultSet result = statement.executeQuery()) {
            if (!result.next() || result.getLong(1) != 42L || result.next())
              throw new IllegalStateException("unexpected query result");
          }
        }
      }
      System.out.println("connector-j-ok");
    } finally {
      Files.deleteIfExists(trustStore);
    }
  }
}
