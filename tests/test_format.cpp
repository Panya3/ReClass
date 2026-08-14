#include <QtTest/QTest>
#include <cstring>
#include "core.h"

using namespace rcx;

class TestFormat : public QObject {
    Q_OBJECT
private slots:
    void testTypeName() {
        QString s = fmt::typeName(NodeKind::Float);
        QVERIFY(s.trimmed() == "float");
        QCOMPARE(s.size(), 14); // kColType
    }

    void testFmtInt32() {
        // fmtInt32 outputs decimal representation
        QCOMPARE(fmt::fmtInt32(-42), QString("-42"));
        QCOMPARE(fmt::fmtInt32(0),   QString("0"));
    }

    void testFmtInt128() {
        // 128-bit formatting (little-endian 16-byte buffer).
        auto fmt128 = [](uint64_t lo, uint64_t hi, bool isSigned) {
            QByteArray b(16, Qt::Uninitialized);
            memcpy(b.data(), &lo, 8);
            memcpy(b.data() + 8, &hi, 8);
            return isSigned ? fmt::fmtInt128(b.constData())
                            : fmt::fmtUInt128(b.constData());
        };
        // Zero
        QCOMPARE(fmt128(0, 0, true),  QString("0"));
        QCOMPARE(fmt128(0, 0, false), QString("0"));
        // Small positive
        QCOMPARE(fmt128(1234567890123ull, 0, true),  QString("1234567890123"));
        // Boundary: 2^64-1 / 2^64
        QCOMPARE(fmt128(0xFFFFFFFFFFFFFFFFull, 0, true), QString("18446744073709551615"));
        QCOMPARE(fmt128(0, 1, true),  QString("18446744073709551616"));
        // Max unsigned: 2^128-1
        QCOMPARE(fmt128(0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull, false),
                 QString("340282366920938463463374607431768211455"));
        // Negative
        QCOMPARE(fmt128(0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull, true),
                 QString("-1"));
        QCOMPARE(fmt128(1, 0xFFFFFFFFFFFFFFFFull, true),
                 QString("-18446744073709551615"));
        // INT128_MIN round-trip: -2^127
        QCOMPARE(fmt128(0, 0x8000000000000000ull, true),
                 QString("-170141183460469231731687303715884105728"));
        // INT128_MAX
        QCOMPARE(fmt128(0xFFFFFFFFFFFFFFFFull, 0x7FFFFFFFFFFFFFFFull, true),
                 QString("170141183460469231731687303715884105727"));
    }

    void testParseInt128() {
        auto parse = [](NodeKind k, const QString& s) {
            bool ok = false;
            QByteArray b = fmt::parseValue(k, s, &ok);
            if (!ok) return QString();
            uint64_t lo, hi;
            memcpy(&lo, b.constData(), 8);
            memcpy(&hi, b.constData() + 8, 8);
            return (k == NodeKind::Int128)
                ? fmt::fmtInt128(b.constData())
                : fmt::fmtUInt128(b.constData());
        };
        // Decimal round-trips
        QCOMPARE(parse(NodeKind::UInt128, "0"), QString("0"));
        QCOMPARE(parse(NodeKind::UInt128, "340282366920938463463374607431768211455"),
                 QString("340282366920938463463374607431768211455"));
        QCOMPARE(parse(NodeKind::Int128, "170141183460469231731687303715884105727"),
                 QString("170141183460469231731687303715884105727"));
        // INT128_MIN must parse
        QCOMPARE(parse(NodeKind::Int128, "-170141183460469231731687303715884105728"),
                 QString("-170141183460469231731687303715884105728"));
        // Hex round-trip (both widths)
        QCOMPARE(parse(NodeKind::UInt128, "0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"),
                 QString("340282366920938463463374607431768211455"));
        // Overflow rejection
        bool ok = true;
        QCOMPARE(fmt::parseValue(NodeKind::UInt128,
                                 "340282366920938463463374607431768211456", &ok),
                 QByteArray());
        QVERIFY(!ok);
        ok = true;
        QCOMPARE(fmt::parseValue(NodeKind::Int128,
                                 "170141183460469231731687303715884105728", &ok),
                 QByteArray());
        QVERIFY(!ok);
        // Negative unsigned rejected
        ok = true;
        QCOMPARE(fmt::parseValue(NodeKind::UInt128, "-1", &ok), QByteArray());
        QVERIFY(!ok);
    }

    void testInt128BigEndian() {
        // Big-endian Int128: Node-aware parseValue byte-swaps, readValue
        // displays from the swapped buffer. Round-trip through a BE node.
        Node n;
        n.kind = NodeKind::Int128;
        n.bigEndian = true;
        bool ok = false;
        QByteArray b = fmt::parseValue(n, QStringLiteral("-2"), &ok);
        QVERIFY(ok);
        // BE storage: high half byte-swapped into first 8 bytes.
        // -2 = 0xFFFF...FFE; BE bytes = FF FF .. FF FE (lo half stored
        // first, already byte-swapped by parseValue).
        QCOMPARE(b.size(), 16);
        // Round-trip through readValueImpl's BE reverse + fmtInt128:
        // the display must come back as "-2".
        // (readValueImpl lives in fmt; exercise via the same path the
        // editor uses — parseValue's reverse is what readValueImpl undoes.)
        QString shown;
        {
            // Simulate readValueImpl BE branch: reverse, then fmtInt128.
            QByteArray rev = b;
            std::reverse(rev.begin(), rev.end());
            shown = fmt::fmtInt128(rev.constData());
        }
        QCOMPARE(shown, QString("-2"));
        // UInt128 BE round-trip, non-symmetric value.
        Node nu;
        nu.kind = NodeKind::UInt128;
        nu.bigEndian = true;
        QByteArray bu = fmt::parseValue(nu, QStringLiteral("340282366920938463463374607431768211455"), &ok);
        QVERIFY(ok);
        QByteArray revu = bu;
        std::reverse(revu.begin(), revu.end());
        QCOMPARE(fmt::fmtUInt128(revu.constData()),
                 QStringLiteral("340282366920938463463374607431768211455"));
    }
    void testFmtFloat() {
        // Positive: 7 chars body. Negative: '-' + 7 chars = 8.
        auto check = [](float v, const char* expected) {
            QString s = fmt::fmtFloat(v);
            QCOMPARE(s, QString(expected));
        };

        // Basic positive/negative
        check( 3.14159f,  "3.1416f");
        check(-3.14159f,  "-3.1416f");

        // Zero
        check( 0.f,       "0.0000f");

        // Small values
        check( 0.02f,     "0.0200f");
        check(-0.069f,    "-0.0690f");

        // Values >= 10 — 3 decimal places
        check( 15.6543f,  "15.654f");
        check(-77.6624f,  "-77.662f");

        // Values >= 100 — 2 decimal places
        check( 500.f,     "500.00f");

        // Values >= 1000 — 1 decimal place
        check( 5000.f,    "5000.0f");

        // Values >= 10000 — 0 decimal places + "."
        check( 50000.f,   "50000.f");

        // Overflow cap
        check( 100000.f,  "99999+f");
        check(-100000.f,  "-99999+f");

        // Special values
        check( INFINITY,  "inff");
        check(-INFINITY,  "-inff");
        QCOMPARE(fmt::fmtFloat(std::nanf("")), QString("NaN"));

        // 1.0 exactly
        check( 1.f,       "1.0000f");
        check(-1.f,       "-1.0000f");
    }

    void testFmtBool() {
        QCOMPARE(fmt::fmtBool(1), QString("true"));
        QCOMPARE(fmt::fmtBool(0), QString("false"));
    }

    void testFmtPointer64_null() {
        QCOMPARE(fmt::fmtPointer64(0), QStringLiteral("nullptr"));
    }

    void testFmtPointer64_nonNull() {
        QString s = fmt::fmtPointer64(0x400000);
        QVERIFY(s.startsWith("0x"));
        QVERIFY(s.contains("400000"));
    }

    void testFmtOffsetMargin_primary() {
        QCOMPARE(fmt::fmtOffsetMargin(0x10, false), QString("00000010 "));
        QCOMPARE(fmt::fmtOffsetMargin(0, false),    QString("00000000 "));
    }

    void testFmtOffsetMargin_continuation() {
        QCOMPARE(fmt::fmtOffsetMargin(0x10, true), QString("  \u00B7 "));
    }

    void testFmtOffsetMargin_kernelAddr() {
        QCOMPARE(fmt::fmtOffsetMargin(0xFFFFF80012345678ULL, false, 16),
                 QString("FFFFF80012345678 "));
        QCOMPARE(fmt::fmtOffsetMargin(0x10, false, 16),
                 QString("0000000000000010 "));
        QCOMPARE(fmt::fmtOffsetMargin(0x10, false, 4),
                 QString("0010 "));
    }

    void testFmtStructHeader() {
        Node n;
        n.kind = NodeKind::Struct;
        n.name = "Test";
        // Expanded header should contain opening brace
        QString s = fmt::fmtStructHeader(n, 0, /*collapsed=*/false);
        QVERIFY(s.contains("struct"));
        QVERIFY(s.contains("Test"));
        QVERIFY(s.contains("{"));

        // Collapsed header should not contain opening brace
        QString collapsed = fmt::fmtStructHeader(n, 0, /*collapsed=*/true);
        QVERIFY(collapsed.contains("struct"));
        QVERIFY(collapsed.contains("Test"));
        QVERIFY(!collapsed.contains("{"));
    }

    void testFmtStructFooter() {
        Node n;
        n.kind = NodeKind::Struct;
        n.name = "Test";
        QString s = fmt::fmtStructFooter(n, 0);
        QVERIFY(s.contains("};"));
        // When no size, footer is just "};" without name
    }

    void testIndent() {
        QCOMPARE(fmt::indent(0), QString(""));
        QCOMPARE(fmt::indent(1), QString("  "));
        QCOMPARE(fmt::indent(3), QString("      "));
    }

    void testParseValueInt32() {
        bool ok;
        QByteArray b = fmt::parseValue(NodeKind::Int32, "-42", &ok);
        QVERIFY(ok);
        QCOMPARE(b.size(), 4);
        int32_t v;
        memcpy(&v, b.data(), 4);
        QCOMPARE(v, -42);
    }

    void testParseValueFloat() {
        bool ok;
        QByteArray b = fmt::parseValue(NodeKind::Float, "3.14", &ok);
        QVERIFY(ok);
        QCOMPARE(b.size(), 4);
        float v;
        memcpy(&v, b.data(), 4);
        QVERIFY(qAbs(v - 3.14f) < 0.01f);
    }

    void testParseValueHex32() {
        bool ok;
        // Hex kinds parse as memory-order bytes (matches hex-preview display).
        // "DEADBEEF" stores bytes [DE, AD, BE, EF] in memory, which round-trips
        // with the hex preview that shows "DE AD BE EF".
        QByteArray b = fmt::parseValue(NodeKind::Hex32, "DEADBEEF", &ok);
        QVERIFY(ok);
        QCOMPARE(b.size(), 4);
        QCOMPARE((uint8_t)b[0], (uint8_t)0xDE);
        QCOMPARE((uint8_t)b[1], (uint8_t)0xAD);
        QCOMPARE((uint8_t)b[2], (uint8_t)0xBE);
        QCOMPARE((uint8_t)b[3], (uint8_t)0xEF);
    }

    void testParseValueBool() {
        bool ok;
        QByteArray b = fmt::parseValue(NodeKind::Bool, "true", &ok);
        QVERIFY(ok);
        QCOMPARE(b.size(), 1);
        QCOMPARE((uint8_t)b[0], (uint8_t)1);

        b = fmt::parseValue(NodeKind::Bool, "false", &ok);
        QVERIFY(ok);
        QCOMPARE((uint8_t)b[0], (uint8_t)0);

        // Unknown token should fail
        fmt::parseValue(NodeKind::Bool, "banana", &ok);
        QVERIFY(!ok);
    }

    void testParseValueHex0xPrefix() {
        bool ok;
        // Hex32 with 0x prefix: prefix is stripped, rest parsed as memory bytes
        QByteArray b = fmt::parseValue(NodeKind::Hex32, "0xDEADBEEF", &ok);
        QVERIFY(ok);
        QCOMPARE(b.size(), 4);
        QCOMPARE((uint8_t)b[0], (uint8_t)0xDE);
        QCOMPARE((uint8_t)b[3], (uint8_t)0xEF);

        // Pointer64 with 0x prefix — pointer kinds still parse as integer
        b = fmt::parseValue(NodeKind::Pointer64, "0x0000000000400000", &ok);
        QVERIFY(ok);
        uint64_t v64;
        memcpy(&v64, b.data(), 8);
        QCOMPARE(v64, (uint64_t)0x400000);
    }

    void testParseValueOverflow() {
        bool ok;
        // UInt8: 300 exceeds uint8_t max (255) → should fail
        fmt::parseValue(NodeKind::UInt8, "300", &ok);
        QVERIFY(!ok);

        // UInt8: 255 should succeed
        QByteArray b = fmt::parseValue(NodeKind::UInt8, "255", &ok);
        QVERIFY(ok);
        QCOMPARE((uint8_t)b[0], (uint8_t)255);

        // Int8: 200 exceeds int8_t max (127) → should fail
        fmt::parseValue(NodeKind::Int8, "200", &ok);
        QVERIFY(!ok);

        // Int8: -129 below min → should fail
        fmt::parseValue(NodeKind::Int8, "-129", &ok);
        QVERIFY(!ok);

        // Int8: -128 is valid
        b = fmt::parseValue(NodeKind::Int8, "-128", &ok);
        QVERIFY(ok);
        int8_t sv;
        memcpy(&sv, b.data(), 1);
        QCOMPARE(sv, (int8_t)-128);

        // UInt16: 70000 exceeds uint16_t max → should fail
        fmt::parseValue(NodeKind::UInt16, "70000", &ok);
        QVERIFY(!ok);

        // Hex8: 0x1FF exceeds uint8_t → should fail
        fmt::parseValue(NodeKind::Hex8, "1FF", &ok);
        QVERIFY(!ok);

        // Hex16: 0x1FFFF exceeds uint16_t → should fail
        fmt::parseValue(NodeKind::Hex16, "1FFFF", &ok);
        QVERIFY(!ok);
    }

    void testSignedHexRoundTrip() {
        bool ok;
        // Int8: 0xFF should parse as -1 (two's complement)
        QByteArray b = fmt::parseValue(NodeKind::Int8, "0xFF", &ok);
        QVERIFY(ok);
        int8_t sv8;
        memcpy(&sv8, b.data(), 1);
        QCOMPARE(sv8, (int8_t)-1);

        // Int8: 0x80 should parse as -128
        b = fmt::parseValue(NodeKind::Int8, "0x80", &ok);
        QVERIFY(ok);
        memcpy(&sv8, b.data(), 1);
        QCOMPARE(sv8, (int8_t)-128);

        // Int16: 0xFFFF should parse as -1
        b = fmt::parseValue(NodeKind::Int16, "0xFFFF", &ok);
        QVERIFY(ok);
        int16_t sv16;
        memcpy(&sv16, b.data(), 2);
        QCOMPARE(sv16, (int16_t)-1);

        // Int32: 0xFFFFFFFF should parse as -1
        b = fmt::parseValue(NodeKind::Int32, "0xFFFFFFFF", &ok);
        QVERIFY(ok);
        int32_t sv32;
        memcpy(&sv32, b.data(), 4);
        QCOMPARE(sv32, (int32_t)-1);

        // Int8: 0x1FF should fail (exceeds byte range)
        fmt::parseValue(NodeKind::Int8, "0x1FF", &ok);
        QVERIFY(!ok);

        // Int16: 0x1FFFF should fail (exceeds 16-bit range)
        fmt::parseValue(NodeKind::Int16, "0x1FFFF", &ok);
        QVERIFY(!ok);
    }

    void testReadValueBoundsCheck() {
        // Vec2 single-line: subLine=0 returns all components
        QByteArray data(16, '\0');
        BufferProvider prov(data);
        Node n;
        n.kind = NodeKind::Vec2;
        n.name = "v";
        QVERIFY(fmt::readValue(n, prov, 0, 0).contains(","));

        // Vec3 single-line: subLine=0 returns 3 comma-separated values
        n.kind = NodeKind::Vec3;
        QCOMPARE(fmt::readValue(n, prov, 0, 0).count(','), 2);

        // Vec4 single-line: subLine=0 returns 4 comma-separated values
        n.kind = NodeKind::Vec4;
        QCOMPARE(fmt::readValue(n, prov, 0, 0).count(','), 3);
    }

    void testEditableValueBasic() {
        QByteArray data(16, '\0');
        // Write a known float value
        float val = 3.14f;
        memcpy(data.data(), &val, 4);
        BufferProvider prov(data);

        Node n;
        n.kind = NodeKind::Float;
        n.name = "f";
        QString s = fmt::editableValue(n, prov, 0, 0);
        QVERIFY(s.contains("3.14"));

        // Vec2 single-line: returns comma-separated values
        n.kind = NodeKind::Vec2;
        QString vec2 = fmt::editableValue(n, prov, 0, 0);
        QVERIFY(vec2.contains(","));
    }

    void testParseValueEmptyString() {
        bool ok;
        // Empty UTF8 should succeed (caller pads)
        QByteArray b = fmt::parseValue(NodeKind::UTF8, "", &ok);
        QVERIFY(ok);
        QVERIFY(b.isEmpty());

        // Empty non-string should fail
        fmt::parseValue(NodeKind::Int32, "", &ok);
        QVERIFY(!ok);
    }

    void testFmtStructFooterSimple() {
        Node n;
        n.kind = NodeKind::Struct;
        n.name = "Test";

        // Footer is always just "};" (no sizeof comment)
        QString s = fmt::fmtStructFooter(n, 0, 0x14);
        QVERIFY(s.contains("};"));
        QVERIFY(!s.contains("sizeof"));  // No sizeof comment
    }
    void testFmtFloatEdgeCases() {
        QCOMPARE(fmt::fmtFloat(std::numeric_limits<float>::quiet_NaN()), QStringLiteral("NaN"));
        QCOMPARE(fmt::fmtFloat(std::numeric_limits<float>::infinity()), QStringLiteral("inff"));
        QCOMPARE(fmt::fmtFloat(-std::numeric_limits<float>::infinity()), QStringLiteral("-inff"));
        // Normal float should contain 'f' suffix
        QVERIFY(fmt::fmtFloat(3.14f).contains('f'));
        // -0.0f should display with minus sign
        QVERIFY(fmt::fmtFloat(-0.0f).startsWith('-'));
    }

    void testFmtDoubleIntegerValue() {
        // Double with integer value should still have decimal point
        QString s = fmt::fmtDouble(42.0);
        QVERIFY(s.contains('.'));
    }

    void testFmtBoolValues() {
        QCOMPARE(fmt::fmtBool(1), QStringLiteral("true"));
        QCOMPARE(fmt::fmtBool(0), QStringLiteral("false"));
    }

    void testValidateValueEmpty() {
        // Empty string should be OK (some contexts allow empty)
        QVERIFY(fmt::validateValue(NodeKind::Int32, "").isEmpty());
    }

    void testValidateValueHexOverflow() {
        // Value too large for Int8 should produce error
        QString err = fmt::validateValue(NodeKind::Int8, "999");
        QVERIFY(!err.isEmpty());
    }

    void testParseValueBoolStrings() {
        bool ok;
        QByteArray b = fmt::parseValue(NodeKind::Bool, "true", &ok);
        QVERIFY(ok);
        QCOMPARE(b.size(), 1);
        QCOMPARE((uint8_t)b[0], (uint8_t)1);

        b = fmt::parseValue(NodeKind::Bool, "false", &ok);
        QVERIFY(ok);
        QCOMPARE((uint8_t)b[0], (uint8_t)0);
    }
    void testParseValueHex128() {
        bool ok;
        // Space-separated 16 bytes
        QByteArray b = fmt::parseValue(NodeKind::Hex128,
            "00 11 22 33 44 55 66 77 88 99 AA BB CC DD EE FF", &ok);
        QVERIFY(ok);
        QCOMPARE(b.size(), 16);
        QCOMPARE((uint8_t)b[0], (uint8_t)0x00);
        QCOMPARE((uint8_t)b[15], (uint8_t)0xFF);
    }

    void testParseValueHex128TooShort() {
        bool ok;
        // Only 8 bytes — should fail (expects 16)
        QByteArray b = fmt::parseValue(NodeKind::Hex128,
            "00 11 22 33 44 55 66 77", &ok);
        QVERIFY(!ok);
    }

    void testReadValueHex128() {
        // Build a 16-byte buffer and read it as Hex128
        QByteArray data(16, '\0');
        data[0] = 0x41;  // 'A'
        data[15] = (char)0xFF;
        BufferProvider prov(data);
        Node n;
        n.kind = NodeKind::Hex128;
        // Display mode should show hex value
        QString val = fmt::readValue(n, prov, 0, 0);
        QVERIFY(!val.isEmpty());
        // Editable mode should show space-separated hex bytes
        QString edit = fmt::editableValue(n, prov, 0, 0);
        QVERIFY(edit.contains(' '));
        QVERIFY(edit.size() >= 47);  // 16*3-1 = 47 chars
    }

    void testFmtFloatVerySmall() {
        // Very small floats should not show "0.0000f" when nonzero
        QString s = fmt::fmtFloat(1e-7f);
        QVERIFY(s.contains('f'));
        QVERIFY(s.size() <= 9);
    }

    void testFmtDoubleVeryLarge() {
        QString s = fmt::fmtDouble(1e308);
        QVERIFY(!s.isEmpty());
        QVERIFY(s.contains('.') || s.contains('e') || s.contains('E'));
    }

    void testFmtDoubleNegativeZero() {
        QString s = fmt::fmtDouble(-0.0);
        // Qt's QString::number may or may not preserve -0.0
        QVERIFY(!s.isEmpty());
    }

    void testFmtDoubleNanInf() {
        QString sNan = fmt::fmtDouble(std::numeric_limits<double>::quiet_NaN());
        QVERIFY(!sNan.isEmpty());
        QString sInf = fmt::fmtDouble(std::numeric_limits<double>::infinity());
        QVERIFY(!sInf.isEmpty());
    }

    void testParseValueUtf8Emoji() {
        bool ok;
        QByteArray b = fmt::parseValue(NodeKind::UTF8, QStringLiteral("\"hello\""), &ok);
        QVERIFY(ok);
        QCOMPARE(b, QByteArray("hello"));
    }

    void testParseValueHex16SpaceSeparated() {
        bool ok;
        QByteArray b = fmt::parseValue(NodeKind::Hex16, "AB CD", &ok);
        QVERIFY(ok);
        QCOMPARE(b.size(), 2);
        QCOMPARE((uint8_t)b[0], (uint8_t)0xAB);
        QCOMPARE((uint8_t)b[1], (uint8_t)0xCD);
    }

    void testValidateValueHex128() {
        QString err = fmt::validateValue(NodeKind::Hex128,
            "00 11 22 33 44 55 66 77 88 99 AA BB CC DD EE FF");
        QVERIFY(err.isEmpty());
    }

    void testKindFromTypeNameUnknown() {
        bool ok = true;
        NodeKind k = kindFromTypeName(QStringLiteral("nonsense"), &ok);
        QVERIFY(!ok);
        QCOMPARE(k, NodeKind::Hex8);
    }

    void testAllTypeNamesForUI() {
        QStringList names = allTypeNamesForUI();
        QCOMPARE(names.size(), (int)std::size(kKindMeta));
        // No duplicates
        QSet<QString> s(names.begin(), names.end());
        QCOMPARE(s.size(), names.size());
    }

    void testIsValidPrimitivePtrTarget() {
        QVERIFY(!isValidPrimitivePtrTarget(NodeKind::Hex8));
        QVERIFY(!isValidPrimitivePtrTarget(NodeKind::Pointer64));
        QVERIFY(!isValidPrimitivePtrTarget(NodeKind::Struct));
        QVERIFY(!isValidPrimitivePtrTarget(NodeKind::FuncPtr64));
        QVERIFY(isValidPrimitivePtrTarget(NodeKind::Int32));
        QVERIFY(isValidPrimitivePtrTarget(NodeKind::Float));
        QVERIFY(isValidPrimitivePtrTarget(NodeKind::Bool));
    }

    void testSupportsHexDisplayToggle() {
        auto base = [](NodeKind k) { Node n; n.kind = k; return n; };

        // Scalar numbers: eligible.
        QVERIFY(supportsHexDisplayToggle(base(NodeKind::UInt32)));
        QVERIFY(supportsHexDisplayToggle(base(NodeKind::Int8)));
        QVERIFY(supportsHexDisplayToggle(base(NodeKind::Int64)));
        QVERIFY(supportsHexDisplayToggle(base(NodeKind::UInt128)));
        QVERIFY(supportsHexDisplayToggle(base(NodeKind::Float16)));
        QVERIFY(supportsHexDisplayToggle(base(NodeKind::Float)));
        QVERIFY(supportsHexDisplayToggle(base(NodeKind::Double)));

        // Not numbers / already-hex / containers: excluded.
        QVERIFY(!supportsHexDisplayToggle(base(NodeKind::Hex32)));
        QVERIFY(!supportsHexDisplayToggle(base(NodeKind::Bool)));
        QVERIFY(!supportsHexDisplayToggle(base(NodeKind::Struct)));
        QVERIFY(!supportsHexDisplayToggle(base(NodeKind::Array)));
        QVERIFY(!supportsHexDisplayToggle(base(NodeKind::UTF8)));
        QVERIFY(!supportsHexDisplayToggle(base(NodeKind::Vec3)));

        // Enum pick field (integer with refId): value renders as a chip
        // pill, so a hex toggle would be a no-op.
        Node enumField = base(NodeKind::UInt32);
        enumField.refId = 42;
        QVERIFY(!supportsHexDisplayToggle(enumField));

        // Typed primitive pointer (64-bit): the deref is a number → eligible.
        Node ptr = base(NodeKind::Pointer64);
        ptr.ptrDepth = 1;
        ptr.elementKind = NodeKind::UInt32;
        QVERIFY(supportsHexDisplayToggle(ptr));

        // Plain / class pointers: the shown value is the address → excluded.
        QVERIFY(!supportsHexDisplayToggle(base(NodeKind::Pointer64)));
        Node classPtr = base(NodeKind::Pointer64);
        classPtr.refId = 7;
        QVERIFY(!supportsHexDisplayToggle(classPtr));

        // Pointer32 never derefs primitives → a toggle there would no-op.
        Node ptr32 = base(NodeKind::Pointer32);
        ptr32.ptrDepth = 1;
        ptr32.elementKind = NodeKind::UInt32;
        QVERIFY(!supportsHexDisplayToggle(ptr32));

        // Pointer to a non-number target (bool) → excluded.
        Node boolPtr = base(NodeKind::Pointer64);
        boolPtr.ptrDepth = 1;
        boolPtr.elementKind = NodeKind::Bool;
        QVERIFY(!supportsHexDisplayToggle(boolPtr));
    }

    // ── Display as Hex (session-only per-node toggle) ──

    void testFmtUIntDefaultDecimal() {
        // Unsigned integer formatters now render decimal by default; hex
        // is the opt-in display mode driven by node.displayHex.
        QCOMPARE(fmt::fmtUInt8(0xAB), QStringLiteral("171"));
        QCOMPARE(fmt::fmtUInt16(0x1234), QStringLiteral("4660"));
        QCOMPARE(fmt::fmtUInt32(5), QStringLiteral("5"));
        QCOMPARE(fmt::fmtUInt32(0xFFFFFFFFu), QStringLiteral("4294967295"));
        QCOMPARE(fmt::fmtUInt64(0xFFFFFFFFFFFFFFFFull), QStringLiteral("18446744073709551615"));
    }

    void testDisplayHexToggle() {
        // UInt32: default decimal, displayHex → hex
        QByteArray data(16, '\0');
        uint32_t v = 0x1A2B3C4D;
        memcpy(data.data(), &v, 4);
        BufferProvider prov(data);

        Node n;
        n.kind = NodeKind::UInt32;
        QCOMPARE(fmt::readValue(n, prov, 0, 0), QStringLiteral("439041101"));
        n.displayHex = true;
        QCOMPARE(fmt::readValue(n, prov, 0, 0), QStringLiteral("0x1a2b3c4d"));
        // Editable form matches the display so the edit dialog round-trips.
        QCOMPARE(fmt::editableValue(n, prov, 0, 0), QStringLiteral("0x1a2b3c4d"));

        // Int32 (positive value): decimal by default, raw bit pattern as
        // hex when toggled.
        n.kind = NodeKind::Int32;
        n.displayHex = false;
        QCOMPARE(fmt::readValue(n, prov, 0, 0), QStringLiteral("439041101"));
        n.displayHex = true;
        QCOMPARE(fmt::readValue(n, prov, 0, 0), QStringLiteral("0x1a2b3c4d"));

        // Float: value by default, raw IEEE-754 bit pattern as hex.
        // (BufferProvider copies its input at construction, so rebuild it
        // after overwriting the buffer.)
        data.fill('\0');
        float f = 1.0f;
        memcpy(data.data(), &f, 4);
        BufferProvider provF(data);
        n.kind = NodeKind::Float;
        n.displayHex = false;
        QVERIFY(fmt::readValue(n, provF, 0, 0).contains("1.0"));
        n.displayHex = true;
        QCOMPARE(fmt::readValue(n, provF, 0, 0), QStringLiteral("0x3f800000"));

        // Double: raw 64-bit pattern.
        data.fill('\0');
        double d = 1.0;
        memcpy(data.data(), &d, 8);
        BufferProvider provD(data);
        n.kind = NodeKind::Double;
        n.displayHex = true;
        QCOMPARE(fmt::readValue(n, provD, 0, 0), QStringLiteral("0x3ff0000000000000"));

        // UInt128: decimal by default, hex when toggled.
        data.fill('\0');
        uint64_t lo = 0xABCDEF0123456789ull, hi = 0xDEADBEEFCAFEBABEull;
        memcpy(data.data(), &lo, 8);
        memcpy(data.data() + 8, &hi, 8);
        BufferProvider provU(data);
        n.kind = NodeKind::UInt128;
        n.displayHex = false;
        QCOMPARE(fmt::readValue(n, provU, 0, 0),
                 QStringLiteral("295990755076957304710458999272422860681"));
        n.displayHex = true;
        QCOMPARE(fmt::readValue(n, provU, 0, 0),
                 QStringLiteral("0xDEADBEEFCAFEBABEABCDEF0123456789"));
    }

    void testParseFloatHexBits() {
        // Hex display mode round-trip: "0x…" parses as the raw bit pattern.
        bool ok = false;
        QByteArray b = fmt::parseValue(NodeKind::Float, QStringLiteral("0x3F800000"), &ok);
        QVERIFY(ok);
        QCOMPARE(b.size(), 4);
        float f;
        memcpy(&f, b.constData(), 4);
        QCOMPARE(f, 1.0f);

        ok = false;
        b = fmt::parseValue(NodeKind::Double, QStringLiteral("0x3FF0000000000000"), &ok);
        QVERIFY(ok);
        QCOMPARE(b.size(), 8);
        double d;
        memcpy(&d, b.constData(), 8);
        QCOMPARE(d, 1.0);

        ok = false;
        b = fmt::parseValue(NodeKind::Float16, QStringLiteral("0x3C00"), &ok);
        QVERIFY(ok);
        QCOMPARE(b.size(), 2);
        QCOMPARE((uint8_t)b[0], (uint8_t)0x00);
        QCOMPARE((uint8_t)b[1], (uint8_t)0x3C);

        // Over-range 16-bit pattern rejected.
        ok = true;
        fmt::parseValue(NodeKind::Float16, QStringLiteral("0x10000"), &ok);
        QVERIFY(!ok);
    }

    void testDisplayHexPointerDeref() {
        // Typed pointer to uint32_t: deref shows decimal by default and
        // hex when the pointer node's displayHex flag is set. The pointer
        // address itself always renders hex.
        QByteArray data(0x110, '\0');
        uint64_t ptr = 0x100;
        memcpy(data.data(), &ptr, 8);
        uint32_t target = 5;
        memcpy(data.data() + 0x100, &target, 4);
        BufferProvider prov(data);

        Node n;
        n.kind = NodeKind::Pointer64;
        n.ptrDepth = 1;
        n.elementKind = NodeKind::UInt32;
        QCOMPARE(fmt::readValue(n, prov, 0, 0), QStringLiteral("-> 5"));
        n.displayHex = true;
        QCOMPARE(fmt::readValue(n, prov, 0, 0), QStringLiteral("-> 0x5"));
        // Plain void* pointer: address stays hex regardless of the flag.
        n.ptrDepth = 0;
        n.elementKind = NodeKind::UInt8;
        QCOMPARE(fmt::readValue(n, prov, 0, 0), QStringLiteral("0x100"));
    }
};

QTEST_MAIN(TestFormat)
#include "test_format.moc"
