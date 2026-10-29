/* Host tests for the .app manifest parser and its helpers (kernel/app/manifest.c). */
#include "test.h"
#include <crug/app.h>
#include <crug/string.h>

/* Manifests are written as line arrays and joined here so the test text can be
   read without escape noise.  A trailing newline is optional on purpose: both
   forms occur in the images scripts/mkdisk.sh builds. */
static usize join_lines(const char *const *lines, u32 n, char *out, usize cap,
                        int trailing_nl)
{
    usize len = 0u;
    u32 i;

    for (i = 0u; i < n; i++) {
        usize l = strlen(lines[i]);

        if (len + l + 2u > cap) {
            return 0u;
        }
        memcpy(out + len, lines[i], l);
        len += l;
        out[len++] = 10;
    }
    if (!trailing_nl && len > 0u) {
        len--;
    }
    out[len] = 0;
    return len;
}

static const struct app_manifest *parse_ok(const char *tag, const char *text,
                                          struct app_manifest *m)
{
    const char *reason = 0;
    int rc = app_manifest_parse(text, strlen(text), m, &reason);

    tests_run++;
    if (rc != 0) {
        test_fail(__FILE__, __LINE__, tag);
        fprintf(stderr, "  %s: rc=%d reason=%s" "\n", tag, rc,
                reason ? reason : "(null)");
        return 0;
    }
    return m;
}

static void expect_reject(const char *tag, const char *text, const char *want)
{
    struct app_manifest m;
    const char *reason = 0;
    int rc = app_manifest_parse(text, strlen(text), &m, &reason);

    tests_run++;
    if (rc != -1 || !reason || strcmp(reason, want) != 0) {
        test_fail(__FILE__, __LINE__, tag);
        fprintf(stderr, "  %s: expected %s, got rc=%d reason=%s" "\n", tag, want,
                rc, reason ? reason : "(null)");
    }
}

/* A baseline manifest every rejection case mutates by one line. */
static const char *const BASE[] = {
    "[app]",
    "id = \"core\"",
    "name = \"Core\"",
    "version = \"2.1.0\"",
    "",
    "[exec]",
    "prog = \"bin/x86_64/core\"",
};
#define BASE_N ((u32)(sizeof(BASE) / sizeof(BASE[0])))

void test_app_functions(void)
{
    static const char *const hello[] = {
        "[app]",
        "id = \"hello\"",
        "name = \"Hello\"",
        "version = \"1.0.0\"",
        "",
        "[exec]",
        "prog = \"bin/x86_64/app_hello\"",
        "",
        "[tooltypes]",
        "GREET = \"world\"",
        "",
        "[icon]",
        "rgb = \"FFB000\"",
    };
    static const char *const orbit[] = {
        "[app]",
        "id = \"orbit\"",
        "name = \"Orbit\"",
        "version = \"1.0.0\"",
        "",
        "[exec]",
        "prog = \"bin/x86_64/app_orbit\"",
        "",
        "[tooltypes]",
        "MODE = \"demo\"",
        "",
        "[icon]",
        "rgb = \"30C0FF\"",
    };
    static const char *const broken[] = {
        "[app]",
        "id = \"broken\"",
        "name = \"Broken\"",
        "",
        "[exec]",
        "prog = \"bin/x86_64/app_hello\"",
    };
    static const char *const no_id[] = { "[app]", "name = \"x\"" };
    static const char *const bad_id[] = {
        "[app]", "id = \"Hello\"", "name = \"x\"", "version = \"1.0.0\"",
        "[exec]", "prog = \"p\"",
    };
    static const char *const dup_id[] = {
        "[app]", "id = \"a\"", "id = \"b\"", "name = \"x\"",
        "version = \"1.0.0\"", "[exec]", "prog = \"p\"",
    };
    static const char *const no_name[] = {
        "[app]", "id = \"a\"", "version = \"1.0.0\"",
        "[exec]", "prog = \"p\"",
    };
    static const char *const bad_name[] = {
        "[app]", "id = \"a\"", "name = \"bad\tname\"",
        "version = \"1.0.0\"", "[exec]", "prog = \"p\"",
    };
    static const char *const bad_version[] = {
        "[app]", "id = \"a\"", "name = \"x\"", "version = \"1.0\"",
        "[exec]", "prog = \"p\"",
    };
    static const char *const no_prog[] = {
        "[app]", "id = \"a\"", "name = \"x\"", "version = \"1.0.0\"",
    };
    static const char *const abs_prog[] = {
        "[app]", "id = \"a\"", "name = \"x\"", "version = \"1.0.0\"",
        "[exec]", "prog = \"/bin/x\"",
    };
    static const char *const dotdot_prog[] = {
        "[app]", "id = \"a\"", "name = \"x\"", "version = \"1.0.0\"",
        "[exec]", "prog = \"bin/../x\"",
    };
    static const char *const empty_prog[] = {
        "[app]", "id = \"a\"", "name = \"x\"", "version = \"1.0.0\"",
        "[exec]", "prog = \"\"",
    };
    static const char *const min_os[] = {
        "[app]", "id = \"a\"", "name = \"x\"", "version = \"1.0.0\"",
        "min_os = \"9.9.9\"", "[exec]", "prog = \"p\"",
    };
    static const char *const unknown_section[] = {
        "[app]", "id = \"a\"", "name = \"x\"", "version = \"1.0.0\"",
        "[exec]", "prog = \"p\"", "[weird]", "k = \"v\"",
    };
    static const char *const unknown_key[] = {
        "[app]", "id = \"a\"", "name = \"x\"", "version = \"1.0.0\"",
        "colour = \"red\"", "[exec]", "prog = \"p\"",
    };
    static const char *const no_equal[] = {
        "[app]", "id \"a\"", "name = \"x\"", "version = \"1.0.0\"",
        "[exec]", "prog = \"p\"",
    };
    static const char *const only_equal[] = {
        "[app]", "id = \"a\"", "name = \"x\"", "version = \"1.0.0\"",
        "[exec]", "prog = \"p\"", "= \"v\"",
    };
    static const char *const trailing_junk[] = {
        "[app]", "id = \"a\" extra", "name = \"x\"", "version = \"1.0.0\"",
        "[exec]", "prog = \"p\"",
    };
    static const char *const unterminated[] = {
        "[app]", "id = \"a\"", "name = \"x\"", "version = \"1.0.0\"",
        "[exec]", "prog = \"p",
    };
    static const char *const short_rgb[] = {
        "[app]", "id = \"a\"", "name = \"x\"", "version = \"1.0.0\"",
        "[exec]", "prog = \"p\"", "[icon]", "rgb = \"FFB00\"",
    };
    static const char *const bad_hex[] = {
        "[app]", "id = \"a\"", "name = \"x\"", "version = \"1.0.0\"",
        "[exec]", "prog = \"p\"", "[icon]", "rgb = \"FFB00G\"",
    };
    static const char *const dup_icon[] = {
        "[app]", "id = \"a\"", "name = \"x\"", "version = \"1.0.0\"",
        "[exec]", "prog = \"p\"", "[icon]", "rgb = \"112233\"",
        "rgb = \"445566\"",
    };
    static const char *const many_tooltypes[] = {
        "[app]", "id = \"a\"", "name = \"x\"", "version = \"1.0.0\"",
        "[exec]", "prog = \"p\"", "[tooltypes]",
        "K0 = \"v\"", "K1 = \"v\"", "K2 = \"v\"", "K3 = \"v\"",
        "K4 = \"v\"", "K5 = \"v\"", "K6 = \"v\"", "K7 = \"v\"",
        "K8 = \"v\"",
    };
    static const char *const long_key[] = {
        "[app]", "id = \"a\"", "name = \"x\"", "version = \"1.0.0\"",
        "[exec]", "prog = \"p\"", "[tooltypes]",
        "ABCDEFGHIJKLMNOP = \"v\"",
    };
    static const char *const long_value[] = {
        "[app]", "id = \"a\"", "name = \"x\"", "version = \"1.0.0\"",
        "[exec]", "prog = \"p\"", "[tooltypes]",
        "K = \"0123456789012345678901234567890X\"",
    };
    static const char *const spaced[] = {
        "   [app]   ",
        "  id   =   \"spaced\"  ",
        "\tname\t=\t\"Tabbed\"\t",
        "version = \"1.2.3\"",
        "[exec]",
        "prog = \"bin/x\"",
    };
    static const char *const commented[] = {
        "# a bundle manifest",
        "",
        "[app]",
        "# the id is the primary key",
        "id = \"c\"",
        "name = \"See\"",
        "version = \"0.1.0\"",
        "",
        "[exec]",
        "prog = \"bin/c\"",
    };
    static const char *const lower_tt[] = {
        "[app]", "id = \"a\"", "name = \"x\"", "version = \"1.0.0\"",
        "[exec]", "prog = \"p\"", "[tooltypes]",
        "lower = \"v\"", "Mixed_Key.1 = \"w\"",
    };
    char buf[APP_MANIFEST_MAX + 8];
    char info[APP_INFO_MAX];
    struct app_manifest m;
    struct app_manifest m2;
    const struct app_tooltype *tt;
    usize len;
    const char *reason = 0;

    /* ---- the bundles scripts/mkdisk.sh actually builds ---- */
    len = join_lines(hello, (u32)(sizeof(hello) / sizeof(hello[0])), buf,
                     sizeof(buf), 1);
    CHECK(len > 0u);
    if (parse_ok("hello manifest", buf, &m)) {
        CHECK_STR(m.id, "hello");
        CHECK_STR(m.name, "Hello");
        CHECK_STR(m.version, "1.0.0");
        CHECK_STR(m.min_os, "");
        CHECK_STR(m.prog, "bin/x86_64/app_hello");
        CHECK(m.icon_rgb == 0x00FFB000u);
        CHECK(m.has_icon == 1);
        CHECK(m.tooltype_count == 1u);
        tt = app_tooltype_find(&m, "GREET");
        CHECK(tt != 0);
        if (tt) {
            CHECK_STR(tt->key, "GREET");
            CHECK_STR(tt->value, "world");
        }
        CHECK(app_tooltype_find(&m, "greet") == 0);
        CHECK(app_tooltype_find(&m, "MODE") == 0);
    }

    /* the same text without a trailing newline parses identically */
    len = join_lines(hello, (u32)(sizeof(hello) / sizeof(hello[0])), buf,
                     sizeof(buf), 0);
    CHECK(len > 0u);
    if (parse_ok("hello without final newline", buf, &m)) {
        CHECK_STR(m.id, "hello");
        CHECK(m.icon_rgb == 0x00FFB000u);
        CHECK(m.tooltype_count == 1u);
    }

    len = join_lines(orbit, (u32)(sizeof(orbit) / sizeof(orbit[0])), buf,
                     sizeof(buf), 1);
    CHECK(len > 0u);
    if (parse_ok("orbit manifest", buf, &m)) {
        CHECK_STR(m.id, "orbit");
        CHECK(m.icon_rgb == 0x0030C0FFu);
        tt = app_tooltype_find(&m, "MODE");
        CHECK(tt != 0);
        if (tt) {
            CHECK_STR(tt->value, "demo");
        }
    }

    join_lines(broken, (u32)(sizeof(broken) / sizeof(broken[0])), buf,
               sizeof(buf), 1);
    expect_reject("broken manifest (no version)", buf, "no-version");
    join_lines(broken, (u32)(sizeof(broken) / sizeof(broken[0])), buf,
               sizeof(buf), 0);
    expect_reject("broken manifest without final newline", buf, "no-version");

    /* ---- padding, comments and tool type keys ---- */
    len = join_lines(spaced, (u32)(sizeof(spaced) / sizeof(spaced[0])), buf,
                     sizeof(buf), 1);
    if (parse_ok("padded keys and values", buf, &m)) {
        CHECK_STR(m.id, "spaced");
        CHECK_STR(m.name, "Tabbed");
        CHECK_STR(m.version, "1.2.3");
    }
    len = join_lines(commented, (u32)(sizeof(commented) / sizeof(commented[0])),
                     buf, sizeof(buf), 1);
    if (parse_ok("comments and blank lines", buf, &m)) {
        CHECK_STR(m.id, "c");
        CHECK(m.icon_rgb == APP_DEFAULT_ICON_RGB);
        CHECK(m.has_icon == 0);
        CHECK(m.tooltype_count == 0u);
    }
    join_lines(lower_tt, (u32)(sizeof(lower_tt) / sizeof(lower_tt[0])), buf,
               sizeof(buf), 1);
    if (parse_ok("lowercase tool type keys", buf, &m)) {
        CHECK(m.tooltype_count == 2u);
        CHECK(app_tooltype_find(&m, "lower") != 0);
        CHECK(app_tooltype_find(&m, "Mixed_Key.1") != 0);
    }

    /* ---- rejections ---- */
    join_lines(BASE, BASE_N, buf, sizeof(buf), 1);
    if (parse_ok("baseline manifest", buf, &m)) {
        CHECK_STR(m.id, "core");
    }
    CHECK(app_manifest_parse(0, 10u, &m, &reason) == -1);
    CHECK(reason != 0 && strcmp(reason, "bad-entry") == 0);
    CHECK(app_manifest_parse("x", 0, &m, &reason) == -1);
    CHECK(reason != 0 && strcmp(reason, "bad-entry") == 0);
    CHECK(app_manifest_parse("x", 1, 0, &reason) == -1);

    memset(buf, 35, APP_MANIFEST_MAX + 1u);
    buf[APP_MANIFEST_MAX + 1u] = 0;
    CHECK(app_manifest_parse(buf, APP_MANIFEST_MAX + 1u, &m, &reason) == -1);
    CHECK(reason != 0 && strcmp(reason, "manifest-too-large") == 0);

    expect_reject("no manifest sections", "just some text", "bad-entry");
    expect_reject("unterminated section header", "[app", "bad-entry");
    expect_reject("trailing text after a section header", "[app] x",
                  "bad-entry");
    join_lines(no_id, (u32)(sizeof(no_id) / sizeof(no_id[0])), buf, sizeof(buf),
               1);
    expect_reject("no id", buf, "no-id");
    join_lines(bad_id, (u32)(sizeof(bad_id) / sizeof(bad_id[0])), buf,
               sizeof(buf), 1);
    expect_reject("uppercase id", buf, "bad-id");
    join_lines(dup_id, (u32)(sizeof(dup_id) / sizeof(dup_id[0])), buf,
               sizeof(buf), 1);
    expect_reject("duplicate id", buf, "dup-key");
    join_lines(no_name, (u32)(sizeof(no_name) / sizeof(no_name[0])), buf,
               sizeof(buf), 1);
    expect_reject("no name", buf, "no-name");
    join_lines(bad_name, (u32)(sizeof(bad_name) / sizeof(bad_name[0])), buf,
               sizeof(buf), 1);
    expect_reject("control character in the name", buf, "bad-name");
    join_lines(bad_version, (u32)(sizeof(bad_version) / sizeof(bad_version[0])),
               buf, sizeof(buf), 1);
    expect_reject("two-part version", buf, "bad-version");
    join_lines(no_prog, (u32)(sizeof(no_prog) / sizeof(no_prog[0])), buf,
               sizeof(buf), 1);
    expect_reject("no prog", buf, "no-prog");
    join_lines(abs_prog, (u32)(sizeof(abs_prog) / sizeof(abs_prog[0])), buf,
               sizeof(buf), 1);
    expect_reject("absolute prog", buf, "bad-prog");
    join_lines(dotdot_prog, (u32)(sizeof(dotdot_prog) / sizeof(dotdot_prog[0])),
               buf, sizeof(buf), 1);
    expect_reject("prog with ..", buf, "bad-prog");
    join_lines(empty_prog, (u32)(sizeof(empty_prog) / sizeof(empty_prog[0])),
               buf, sizeof(buf), 1);
    expect_reject("empty prog", buf, "bad-prog");
    join_lines(min_os, (u32)(sizeof(min_os) / sizeof(min_os[0])), buf,
               sizeof(buf), 1);
    expect_reject("min_os newer than the kernel", buf, "min-os");
    join_lines(unknown_section,
               (u32)(sizeof(unknown_section) / sizeof(unknown_section[0])), buf,
               sizeof(buf), 1);
    expect_reject("unknown section", buf, "bad-entry");
    join_lines(unknown_key, (u32)(sizeof(unknown_key) / sizeof(unknown_key[0])),
               buf, sizeof(buf), 1);
    expect_reject("unknown key", buf, "bad-entry");
    join_lines(no_equal, (u32)(sizeof(no_equal) / sizeof(no_equal[0])), buf,
               sizeof(buf), 1);
    expect_reject("line without =", buf, "bad-entry");
    join_lines(only_equal, (u32)(sizeof(only_equal) / sizeof(only_equal[0])),
               buf, sizeof(buf), 1);
    expect_reject("line that starts with =", buf, "bad-entry");
    join_lines(trailing_junk,
               (u32)(sizeof(trailing_junk) / sizeof(trailing_junk[0])), buf,
               sizeof(buf), 1);
    expect_reject("trailing junk after a value", buf, "bad-entry");
    join_lines(unterminated, (u32)(sizeof(unterminated) / sizeof(unterminated[0])),
               buf, sizeof(buf), 1);
    expect_reject("unterminated value", buf, "bad-entry");
    join_lines(short_rgb, (u32)(sizeof(short_rgb) / sizeof(short_rgb[0])), buf,
               sizeof(buf), 1);
    expect_reject("five-digit colour", buf, "bad-rgb");
    join_lines(bad_hex, (u32)(sizeof(bad_hex) / sizeof(bad_hex[0])), buf,
               sizeof(buf), 1);
    expect_reject("non-hex colour", buf, "bad-rgb");
    join_lines(dup_icon, (u32)(sizeof(dup_icon) / sizeof(dup_icon[0])), buf,
               sizeof(buf), 1);
    expect_reject("two icon colours", buf, "dup-key");
    join_lines(many_tooltypes,
               (u32)(sizeof(many_tooltypes) / sizeof(many_tooltypes[0])), buf,
               sizeof(buf), 1);
    expect_reject("nine tool types", buf, "too-many-tooltypes");
    join_lines(long_key, (u32)(sizeof(long_key) / sizeof(long_key[0])), buf,
               sizeof(buf), 1);
    expect_reject("16-byte tool type key", buf, "bad-tooltype");
    join_lines(long_value, (u32)(sizeof(long_value) / sizeof(long_value[0])),
               buf, sizeof(buf), 1);
    expect_reject("32-byte tool type value", buf, "bad-tooltype");

    /* ---- paths ---- */
    CHECK(app_path_join(buf, sizeof(buf), "/Applications/Hello.app",
                        "bin/x86_64/hello") == 0);
    CHECK_STR(buf, "/Applications/Hello.app/bin/x86_64/hello");
    CHECK(app_path_join(buf, sizeof(buf), "/Applications/Hello.app", "") == 0);
    CHECK_STR(buf, "/Applications/Hello.app");
    CHECK(app_path_join(buf, sizeof(buf), "/Applications/Hello.app", 0) == 0);
    CHECK_STR(buf, "/Applications/Hello.app");
    CHECK(app_path_join(buf, sizeof(buf), "/Applications/Hello.app",
                        "/bin/x") == -1);
    CHECK(app_path_join(buf, sizeof(buf), "/Applications/Hello.app",
                        "bin/../x") == -1);
    CHECK(app_path_join(buf, 20u, "/Applications/Hello.app", "bin/x") == -1);
    CHECK(app_path_join(buf, 0u, "/Applications/Hello.app", "x") == -1);
    CHECK(app_path_join(buf, sizeof(buf), "", "x") == -1);
    CHECK(app_path_join(0, 8u, "/a", "b") == -1);

    /* ---- versions ---- */
    CHECK(app_version_compare("1.0.0", "1.0.0") == 0);
    CHECK(app_version_compare("1.0.0", "1.0.1") == -1);
    CHECK(app_version_compare("1.0.1", "1.0.0") == 1);
    CHECK(app_version_compare("3.9.9", "3.10.0") == -1);
    CHECK(app_version_compare("10.0.0", "9.99.99") == 1);
    CHECK(app_version_compare("1.0", "1.0.0") == -2);
    CHECK(app_version_compare("1.0.0.0", "1.0.0") == -2);
    CHECK(app_version_compare("a.b.c", "1.0.0") == -2);
    CHECK(app_version_compare(0, "1.0.0") == -2);
    CHECK(app_version_compare("", "1.0.0") == -2);
    CHECK(app_version_ge("1.2.0", "1.2.0") == 1);
    CHECK(app_version_ge("1.3.0", "1.2.9") == 1);
    CHECK(app_version_ge("1.2.0", "1.3.0") == 0);
    CHECK(app_version_ge("1.2", "1.2.0") == -2);

    /* ---- the SYS_APP_INFO record ---- */
    memset(&m2, 0, sizeof(m2));
    strncpy(m2.id, "hello", sizeof(m2.id));
    strncpy(m2.name, "Hello", sizeof(m2.name));
    strncpy(m2.version, "1.0.0", sizeof(m2.version));
    m2.icon_rgb = 0x00FFB000u;
    strncpy(m2.tooltypes[0].key, "GREET", sizeof(m2.tooltypes[0].key));
    strncpy(m2.tooltypes[0].value, "world", sizeof(m2.tooltypes[0].value));
    strncpy(m2.tooltypes[1].key, "MODE", sizeof(m2.tooltypes[1].key));
    strncpy(m2.tooltypes[1].value, "demo", sizeof(m2.tooltypes[1].value));
    m2.tooltype_count = 2u;
    CHECK(app_format_info(&m2, "/Applications/Hello.app/bin/x86_64/app_hello",
                          info, sizeof(info)) ==
          (int)strlen("hello|Hello|1.0.0"
                      "|/Applications/Hello.app/bin/x86_64/app_hello"
                      "|FFB000|GREET=world;MODE=demo"));
    CHECK_STR(info, "hello|Hello|1.0.0|/Applications/Hello.app/bin/x86_64/app_hello"
                   "|FFB000|GREET=world;MODE=demo");
    m2.tooltype_count = 0u;
    CHECK(app_format_info(&m2, "/x", info, sizeof(info)) ==
          (int)strlen("hello|Hello|1.0.0|/x|FFB000|"));
    CHECK_STR(info, "hello|Hello|1.0.0|/x|FFB000|");
    CHECK(app_format_info(&m2, "/x", info, 4u) == -1);
    CHECK(app_format_info(&m2, "/x", info, 0u) == -1);
    CHECK(app_format_info(0, "/x", info, sizeof(info)) == -1);
    CHECK(app_format_info(&m2, 0, info, sizeof(info)) == -1);
    CHECK(app_format_info(&m2, "/x", 0, sizeof(info)) == -1);

    /* ---- tool type lookup on empty and NULL inputs ---- */
    CHECK(app_tooltype_find(0, "GREET") == 0);
    CHECK(app_tooltype_find(&m2, 0) == 0);
    CHECK(app_tooltype_find(&m2, "GREET") == 0);
}
