package cn.crossdesk.mobile;

import java.util.Locale;

/** Validates and normalizes signaling server addresses (host or host/path) matching minirtc. */
final class ServerAddress {
    private ServerAddress() {}

    static String normalize(String raw) {
        if (raw == null) return null;
        String input = raw.trim();
        if (input.isEmpty() || input.length() >= 256) return null;
        for (int i = 0; i < input.length(); i++) {
            char c = input.charAt(i);
            if (c <= 32 || c == 127 || c == '\\' || c == '#') return null;
        }
        int schemeEnd = input.indexOf("://");
        int slashOrQuery = firstSlashOrQuery(input);
        if (schemeEnd >= 0 && (slashOrQuery < 0 || schemeEnd < slashOrQuery)) {
            String scheme = input.substring(0, schemeEnd).toLowerCase(Locale.ROOT);
            if (!scheme.equals("wss") && !scheme.equals("https")) return null;
            input = input.substring(schemeEnd + 3);
            slashOrQuery = firstSlashOrQuery(input);
        }
        String host = slashOrQuery < 0 ? input : input.substring(0, slashOrQuery);
        String target = slashOrQuery < 0 ? "" : input.substring(slashOrQuery);
        if (host.isEmpty() || host.length() > 253 || !host.matches("[A-Za-z0-9.-]+")
                || host.startsWith(".") || host.endsWith(".")) {
            return null;
        }
        if (!target.isEmpty()) {
            if (target.charAt(0) == '?') target = "/" + target;
            if (target.equals("/")) target = "";
        }
        String normalized = host + target;
        return normalized.length() < 256 ? normalized : null;
    }

    private static int firstSlashOrQuery(String s) {
        int slash = s.indexOf('/');
        int query = s.indexOf('?');
        if (slash < 0) return query;
        if (query < 0) return slash;
        return Math.min(slash, query);
    }
}
