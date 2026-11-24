package cn.crossdesk.mobile;

import org.junit.Test;
import static org.junit.Assert.*;

public class ServerAddressTest {
    @Test public void acceptsHostAndOptionalPath() {
        assertEquals("example.test", ServerAddress.normalize("example.test"));
        assertEquals("example.test", ServerAddress.normalize("  example.test/  "));
        assertEquals("example.test/secret-path", ServerAddress.normalize("example.test/secret-path"));
        assertEquals("example.test/a/b", ServerAddress.normalize("wss://example.test/a/b"));
        assertEquals("example.test/a/b", ServerAddress.normalize("HTTPS://example.test/a/b"));
        assertEquals("192.0.2.10/ws", ServerAddress.normalize("192.0.2.10/ws"));
    }

    @Test public void rejectsInvalidAddresses() {
        assertNull(ServerAddress.normalize(""));
        assertNull(ServerAddress.normalize(".example.test"));
        assertNull(ServerAddress.normalize("example.test."));
        assertNull(ServerAddress.normalize("http://example.test/path"));
        assertNull(ServerAddress.normalize("ws://example.test/path"));
        assertNull(ServerAddress.normalize("example.test/has space"));
        assertNull(ServerAddress.normalize("example.test/path#frag"));
        assertNull(ServerAddress.normalize("user@example.test/path"));
    }
}
