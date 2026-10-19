/* Independent J3 profile assertions. Signing and verification belong to Nimbus.
 * This is a synthetic test harness, not a production JWT or OAuth service.
 */
import com.nimbusds.jose.JOSEException;
import com.nimbusds.jose.JOSEObjectType;
import com.nimbusds.jose.JWSAlgorithm;
import com.nimbusds.jose.JWSHeader;
import com.nimbusds.jose.JWSObject;
import com.nimbusds.jose.Payload;
import com.nimbusds.jose.crypto.ECDSASigner;
import com.nimbusds.jose.crypto.ECDSAVerifier;
import com.nimbusds.jose.jwk.Curve;
import com.nimbusds.jose.jwk.ECKey;
import com.nimbusds.jose.jwk.gen.ECKeyGenerator;
import com.nimbusds.jose.shaded.gson.Strictness;
import com.nimbusds.jose.shaded.gson.stream.JsonReader;
import com.nimbusds.jose.shaded.gson.stream.JsonToken;
import com.nimbusds.jose.util.Base64URL;
import com.nimbusds.jose.util.JSONObjectUtils;

import java.io.IOException;
import java.io.StringReader;
import java.math.BigDecimal;
import java.math.BigInteger;
import java.nio.ByteBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.nio.file.attribute.PosixFilePermissions;
import java.security.MessageDigest;
import java.text.ParseException;
import java.util.ArrayList;
import java.util.Base64;
import java.util.HexFormat;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;

public class AttestationInterop {
    static final long NOW = 1800000100L;
    static final long MAX_DATE = 253402300799L;
    static final String KID = "interop-key";
    static final String TYP = "forge-attestation+jwt";
    static final String ISSUER = "https://issuer.example.test";
    static final String AUDIENCE = "https://resource.example.test";
    static final String PURPOSE = "artifact.read";
    static final String SCHEMA = "https://schemas.example.test/grant/v1";
    static final BigInteger UINT64_MAX = new BigInteger("18446744073709551615");
    static final JWSHeader SIGNING_HEADER = new JWSHeader.Builder(JWSAlgorithm.ES256)
        .type(new JOSEObjectType(TYP)).keyID(KID).build();

    static class InvalidCredential extends Exception { }

    static void require(boolean condition) throws InvalidCredential {
        if (!condition) throw new InvalidCredential();
    }

    static byte[] boundedRead(Path path, int limit) throws Exception {
        try (var stream = Files.newInputStream(path)) {
            byte[] raw = stream.readNBytes(limit + 1);
            require(raw.length <= limit);
            return raw;
        }
    }

    static String utf8(byte[] raw) throws InvalidCredential {
        try {
            return StandardCharsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(raw)).toString();
        } catch (CharacterCodingException error) {
            throw new InvalidCredential();
        }
    }

    static String text(Object value, int minimum, int maximum) throws InvalidCredential {
        require(value instanceof String);
        String result = (String) value;
        require(result.indexOf(0) < 0);
        for (int i = 0; i < result.length(); ++i) {
            char c = result.charAt(i);
            if (Character.isHighSurrogate(c)) {
                require(i + 1 < result.length() && Character.isLowSurrogate(result.charAt(++i)));
            } else {
                require(!Character.isLowSurrogate(c));
            }
        }
        int bytes = result.getBytes(StandardCharsets.UTF_8).length;
        require(bytes >= minimum && bytes <= maximum);
        return result;
    }

    static String productText(Object value, int minimum, int maximum) throws InvalidCredential {
        // Validate Unicode and the wire byte cap before counting scalar values.
        String result = text(value, 0, 2048);
        int scalars = result.codePointCount(0, result.length());
        require(scalars >= minimum && scalars <= maximum);
        return result;
    }

    // Gson's streaming parser supplies tokenization; this small test-only walk
    // adds duplicate-key, lexical number, Unicode and container-bound assertions.
    static Object readJson(JsonReader reader, int depth, int[] nodes, boolean bounded) throws Exception {
        if (bounded) require(++nodes[0] <= 512);
        JsonToken token = reader.peek();
        if (token == JsonToken.BEGIN_OBJECT) {
            require(depth <= (bounded ? 8 : 16));
            reader.beginObject();
            Map<String, Object> result = new LinkedHashMap<>();
            while (reader.hasNext()) {
                String key = reader.nextName();
                if (bounded) text(key, 0, 2048);
                require(!result.containsKey(key));
                require(!bounded || result.size() < 128);
                result.put(key, readJson(reader, depth + 1, nodes, bounded));
            }
            reader.endObject();
            return result;
        }
        if (token == JsonToken.BEGIN_ARRAY) {
            require(depth <= (bounded ? 8 : 16));
            reader.beginArray();
            List<Object> result = new ArrayList<>();
            while (reader.hasNext()) {
                require(!bounded || result.size() < 128);
                result.add(readJson(reader, depth + 1, nodes, bounded));
            }
            reader.endArray();
            return result;
        }
        if (token == JsonToken.STRING) {
            String value = reader.nextString();
            return bounded ? text(value, 0, 2048) : value;
        }
        if (token == JsonToken.NUMBER) {
            String number = reader.nextString();
            if (number.matches("-?(0|[1-9][0-9]*)")) return new BigInteger(number);
            return new BigDecimal(number);
        }
        if (token == JsonToken.BOOLEAN) return reader.nextBoolean();
        if (token == JsonToken.NULL) {
            reader.nextNull();
            return null;
        }
        throw new InvalidCredential();
    }

    static Object parse(byte[] raw, boolean bounded) throws Exception {
        String content = utf8(raw);
        // JsonReader otherwise accepts a leading BOM, outside this strict profile.
        require(content.isEmpty() || content.charAt(0) != '\ufeff');
        try (JsonReader reader = new JsonReader(new StringReader(content))) {
            reader.setStrictness(Strictness.STRICT);
            Object result = readJson(reader, 1, new int[] {0}, bounded);
            require(reader.peek() == JsonToken.END_DOCUMENT);
            return result;
        } catch (IOException | IllegalStateException | NumberFormatException error) {
            throw new InvalidCredential();
        }
    }

    @SuppressWarnings("unchecked")
    static Map<String, Object> object(Object value) throws InvalidCredential {
        require(value instanceof Map);
        return (Map<String, Object>) value;
    }

    static void fields(Map<String, Object> value, String... names) throws InvalidCredential {
        require(value.keySet().equals(Set.of(names)));
    }

    static byte[] segment(String value, int maximum) throws InvalidCredential {
        require(value.matches("[A-Za-z0-9_-]+") && value.length() <= (maximum * 8 + 5) / 6);
        try {
            byte[] result = Base64.getUrlDecoder().decode(value);
            require(result.length <= maximum && Base64URL.encode(result).toString().equals(value));
            return result;
        } catch (IllegalArgumentException error) {
            throw new InvalidCredential();
        }
    }

    static ECKey publicKey(Path path) throws Exception {
        Map<String, Object> key = object(parse(boundedRead(path, 4096), true));
        require(Set.of("kty", "crv", "x", "y", "kid", "alg", "use", "key_ops").containsAll(key.keySet()));
        require(key.keySet().containsAll(Set.of("kty", "crv", "x", "y")));
        require("EC".equals(key.get("kty")) && "P-256".equals(key.get("crv")));
        require(KID.equals(key.getOrDefault("kid", KID)) && "ES256".equals(key.getOrDefault("alg", "ES256")));
        require("sig".equals(key.getOrDefault("use", "sig")));
        require(List.of("verify").equals(key.getOrDefault("key_ops", List.of("verify"))));
        require(segment(text(key.get("x"), 1, 128), 32).length == 32);
        require(segment(text(key.get("y"), 1, 128), 32).length == 32);
        return ECKey.parse(key);
    }

    static void header(Map<String, Object> value) throws InvalidCredential {
        fields(value, "alg", "typ", "kid");
        require("ES256".equals(value.get("alg")) && TYP.equals(value.get("typ")));
        String kid = text(value.get("kid"), 1, 128);
        require(kid.matches("[A-Za-z0-9._-]+") && KID.equals(kid));
    }

    static long date(Object value) throws InvalidCredential {
        require(value instanceof BigInteger);
        BigInteger integer = (BigInteger) value;
        require(integer.signum() >= 0 && integer.compareTo(BigInteger.valueOf(MAX_DATE)) <= 0);
        return integer.longValue();
    }

    static void counter(Object value) throws InvalidCredential {
        String number = text(value, 1, 20);
        require(number.matches("0|[1-9][0-9]*"));
        require(new BigInteger(number).compareTo(UINT64_MAX) <= 0);
    }

    static void document(Map<String, Object> value) throws InvalidCredential {
        fields(value, "iss", "aud", "jti", "iat", "nbf", "exp", "att");
        require(ISSUER.equals(text(value.get("iss"), 1, 256)));
        require(AUDIENCE.equals(text(value.get("aud"), 1, 256)));
        require(text(value.get("jti"), 66, 66).matches("g_[0-9a-f]{64}"));
        long iat = date(value.get("iat")), nbf = date(value.get("nbf")), exp = date(value.get("exp"));
        require(iat <= nbf && nbf < exp && exp - iat <= 300);
        require(iat <= NOW && nbf <= NOW && NOW < exp); // No expiry leeway.
        Map<String, Object> att = object(value.get("att"));
        fields(att, "v", "purpose", "schema", "claims");
        require(BigInteger.ONE.equals(att.get("v")));
        require(PURPOSE.equals(text(att.get("purpose"), 1, 64)));
        require(SCHEMA.equals(text(att.get("schema"), 1, 256)));
        Map<String, Object> claims = object(att.get("claims"));
        fields(claims, "object", "generation", "ranges", "note");
        productText(claims.get("object"), 1, 128);
        productText(claims.get("note"), 1, 128);
        counter(claims.get("generation"));
        require(claims.get("ranges") instanceof List);
        List<?> ranges = (List<?>) claims.get("ranges");
        require(ranges.size() >= 1 && ranges.size() <= 8);
        for (Object item : ranges) {
            Map<String, Object> range = object(item);
            fields(range, "offset", "length");
            counter(range.get("offset"));
            counter(range.get("length"));
        }
    }

    static Map<String, Object> verify(Path tokenPath, Path keyPath) throws Exception {
        byte[] raw = boundedRead(tokenPath, 16384);
        for (byte b : raw) require(b > 0 && b < 127);
        String compact = new String(raw, StandardCharsets.US_ASCII);
        String[] parts = compact.split("\\.", -1);
        require(parts.length == 3);
        byte[] protectedBytes = segment(parts[0], 512), payload = segment(parts[1], 8192);
        require(segment(parts[2], 64).length == 64);
        Map<String, Object> protectedHeader = object(parse(protectedBytes, true));
        header(protectedHeader);
        Map<String, Object> claims = object(parse(payload, true));
        ECKey key = publicKey(keyPath);
        JWSObject signed = JWSObject.parse(compact);
        require(signed.verify(new ECDSAVerifier(key.toECPublicKey())));
        require(java.util.Arrays.equals(signed.getPayload().toBytes(), payload));
        // Only authenticated data reaches the independent common/product checks.
        document(claims);
        return claims;
    }

    static boolean accepted(Path token, Path key) throws Exception {
        try {
            verify(token, key);
            return true;
        } catch (InvalidCredential | ParseException | JOSEException error) {
            return false;
        }
    }

    static String sha256(Path path) throws Exception {
        return HexFormat.of().formatHex(MessageDigest.getInstance("SHA-256").digest(Files.readAllBytes(path)));
    }

    static void write(Path path, byte[] raw) throws Exception {
        Files.write(path, raw, StandardOpenOption.CREATE_NEW);
        Files.setPosixFilePermissions(path, PosixFilePermissions.fromString("rw-------"));
    }

    static void writeJson(Path path, Map<String, Object> value) throws Exception {
        write(path, JSONObjectUtils.toJSONString(value).getBytes(StandardCharsets.UTF_8));
    }

    static Path artifact(Path root, Object relative) throws InvalidCredential {
        Path result = root.resolve(text(relative, 1, 2048)).normalize();
        require(result.startsWith(root));
        return result;
    }

    static void verifyManifest(Path root, Path path) throws Exception {
        Map<String, Object> manifest = object(parse(boundedRead(path, 8 * 1024 * 1024), false));
        require(BigInteger.valueOf(NOW).equals(manifest.get("now")));
        for (Object item : (List<?>) manifest.get("cases")) {
            Map<String, Object> c = object(item);
            Path token = artifact(root, c.get("token")), key = artifact(root, c.get("jwk"));
            require(sha256(token).equals(c.get("token_sha256")) && sha256(key).equals(c.get("jwk_sha256")));
            require(Boolean.valueOf(accepted(token, key)).equals(c.get("accept")));
        }
    }

    static Map<String, Object> expectedDocument() throws Exception {
        String json = """
            {"iss":"https://issuer.example.test","aud":"https://resource.example.test",
             "jti":"g_0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
             "iat":1800000000,"nbf":1800000000,"exp":1800000300,
             "att":{"v":1,"purpose":"artifact.read","schema":"https://schemas.example.test/grant/v1",
             "claims":{"object":"o_17","generation":"18446744073709551615",
             "ranges":[{"offset":"9007199254740993","length":"1"}],"note":"\u043c\u0435\u0434\u0438\u0430"}}}
            """;
        return object(parse(json.getBytes(StandardCharsets.UTF_8), true));
    }

    static void emit(Path root) throws Exception {
        Map<String, Object> forge = verify(root.resolve("forge.jwt"), root.resolve("public.jwk"));
        Map<String, Object> expected = expectedDocument();
        expected.put("jti", forge.get("jti"));
        require(expected.equals(forge));
        Path output = Files.createDirectory(root.resolve("java"));
        Files.setPosixFilePermissions(output, PosixFilePermissions.fromString("rwx------"));
        ECKey signingKey = new ECKeyGenerator(Curve.P_256).keyID(KID).generate();
        ECKey otherKey = new ECKeyGenerator(Curve.P_256).keyID(KID).generate();
        writeJson(output.resolve("public.jwk"), signingKey.toPublicJWK().toJSONObject());
        writeJson(output.resolve("wrong.jwk"), otherKey.toPublicJWK().toJSONObject());
        ECDSASigner signer = new ECDSASigner(signingKey);
        ECDSAVerifier rawVerifier = new ECDSAVerifier(signingKey.toECPublicKey());
        Object vectors = parse(boundedRead(root.resolve("vectors.json"), 8 * 1024 * 1024), false);
        List<Object> cases = new ArrayList<>();
        for (Object item : (List<?>) vectors) {
            Map<String, Object> c = object(item);
            String id = (String) c.get("id"), mode = (String) c.get("mode");
            String input = c.get("header") + "." + c.get("payload");
            byte[] signingInput = input.getBytes(StandardCharsets.US_ASCII);
            // Nimbus signs the exact bytes even for adversarial raw JSON.
            Base64URL signature = signer.sign(SIGNING_HEADER, signingInput);
            require(rawVerifier.verify(SIGNING_HEADER, signingInput, signature));
            String token = input + "." + signature;
            if ("valid".equals(id)) {
                JWSObject normal = new JWSObject(SIGNING_HEADER, new Payload(expectedDocument()));
                normal.sign(signer);
                token = normal.serialize();
            }
            String[] parts = token.split("\\.", -1);
            switch (mode) {
                case "tampered" -> {
                    String payload = utf8(Base64.getUrlDecoder().decode(parts[1]));
                    parts[1] = Base64URL.encode(payload.replace("\"o_17\"", "\"o_18\"")).toString();
                    token = String.join(".", parts);
                }
                case "padding" -> { parts[0] += "="; token = String.join(".", parts); }
                case "noncanonical" -> {
                    String alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
                    token = token.substring(0, token.length() - 1) + alphabet.charAt(alphabet.indexOf(token.charAt(token.length() - 1)) | 1);
                }
                case "extra-segment" -> token += ".eA";
                case "empty-payload" -> token = parts[0] + ".." + parts[2];
                case "short-signature" -> token = input + "." + Base64URL.encode(java.util.Arrays.copyOf(signature.decode(), 63));
                case "signed", "wrong-key" -> { }
                default -> throw new InvalidCredential();
            }
            Path tokenPath = output.resolve(id + ".jwt");
            Path keyPath = output.resolve("wrong-key".equals(mode) ? "wrong.jwk" : "public.jwk");
            write(tokenPath, token.getBytes(StandardCharsets.US_ASCII));
            require(Boolean.valueOf(accepted(tokenPath, keyPath)).equals(c.get("accept")));
            Map<String, Object> result = new LinkedHashMap<>();
            result.put("id", id); result.put("accept", c.get("accept")); result.put("mode", mode);
            result.put("token", root.relativize(tokenPath).toString()); result.put("jwk", root.relativize(keyPath).toString());
            result.put("token_sha256", sha256(tokenPath)); result.put("jwk_sha256", sha256(keyPath));
            cases.add(result);
        }
        Map<String, Object> manifest = new LinkedHashMap<>();
        manifest.put("version", 1); manifest.put("producer", "nimbus-jose-jwt-10.5"); manifest.put("now", NOW);
        manifest.put("verified_forge", true); manifest.put("forge_sha256", sha256(root.resolve("forge.jwt")));
        manifest.put("public_sha256", sha256(root.resolve("public.jwk")));
        manifest.put("vectors_sha256", sha256(root.resolve("vectors.json"))); manifest.put("cases", cases);
        manifest.put("schema_sha256", sha256(root.resolve("document.schema.json")));
        writeJson(output.resolve("manifest.json"), manifest);
    }

    public static void main(String[] args) {
        try {
            require(args.length >= 2);
            Path root = Path.of(args[1]).toAbsolutePath().normalize();
            if ("emit".equals(args[0])) {
                require(args.length == 2);
                emit(root);
            } else {
                require("verify-set".equals(args[0]) && args.length == 3);
                verifyManifest(root, Path.of(args[2]));
            }
            System.out.println("J3 PASS");
        } catch (Exception error) {
            System.err.println("J3 HARNESS_ERROR (details suppressed)");
            System.exit(2);
        }
    }
}
