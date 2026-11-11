/* Host tests for the settings app's log parsers and the store-only zip writer. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "volte_settings_parse.h"
#include "zip_writer.h"

static void test_request(void)
{
    char mode[16], request[32];
    int refresh = 0, debug = 0;

    assert(volte_settings_parse_request("mode=sip-learned\nrefresh=1\nrequest=42\n",
                                        mode, sizeof(mode), request, sizeof(request),
                                        &refresh, &debug) == 1);
    assert(strcmp(mode, "sip-learned") == 0);
    assert(strcmp(request, "42") == 0);
    assert(refresh == 1 && debug == 0);

    assert(volte_settings_parse_request("debug=1\nrequest=7\n", mode, sizeof(mode),
                                        request, sizeof(request), &refresh, &debug) == 1);
    assert(strcmp(mode, "") == 0);
    assert(strcmp(request, "7") == 0);
    assert(debug == 1 && refresh == 0);

    /* No request stamp: not a new request. */
    assert(volte_settings_parse_request("mode=tel\n", mode, sizeof(mode), request,
                                        sizeof(request), &refresh, &debug) == 0);
    assert(volte_settings_parse_request(NULL, mode, sizeof(mode), request, sizeof(request),
                                        &refresh, &debug) == 0);
}

static void test_learned_domain(void)
{
    const char *slog =
        "Nov 10 17:23:17.416 ims_rrm sip INVITE sip:<hidden>@ims.mnc001.mcc262.3gppnetwork.org SIP/2.0\n"
        "Nov 10 17:23:17.416 ims_rrm sip P-Associated-URI: <sip:+491234@ims.telekom.de>, <tel:+491234>\n"
        "Nov 10 17:23:17.416 ims_rrm sip <uri>sip:+491234@ims.telekom.de</uri>\n"
        "Nov 10 17:23:17.416 ims_rrm sip From: <sip:+491234@ims.telekom.de>;tag=x\n";
    char out[128];

    assert(volte_settings_parse_learned_domain(slog, out, sizeof(out)) == 1);
    assert(strcmp(out, "ims.telekom.de") == 0);

    /* Only an IMSI domain: used as the fallback, so the toggle works on carriers whose
       IMPU lives in ims.mnc...3gppnetwork.org. Anonymous is still skipped. */
    assert(volte_settings_parse_learned_domain(
               "From: <sip:262011234567890@ims.mnc001.mcc262.3gppnetwork.org>\n"
               "From: <sip:anonymous@anonymous.invalid>\n",
               out, sizeof(out)) == 1);
    assert(strcmp(out, "ims.mnc001.mcc262.3gppnetwork.org") == 0);
    assert(volte_settings_parse_learned_domain("", out, sizeof(out)) == 0);
    assert(volte_settings_parse_learned_domain(NULL, out, sizeof(out)) == 0);
}

static void test_last_call(void)
{
    const char *failed =
        "createOutDialog(): R-URI: sip:+491234@ims.telekom.de;user=phone\n"
        "SIP/2.0 100 Trying\n"
        "Reason: SIP;cause=503;text=\"Service Unavailable (1:223)\"\n"
        "onDialogEnded(): call-id x, reason 5, code 503\n";
    const char *connected =
        "createOutDialog(): R-URI: tel:+391234\n"
        "onDialogStateChange() 2 => 4\n"
        "onDialogEnded(): call-id x, reason 3, code 16\n";
    char result[48], uri[64];

    volte_settings_parse_last_call(failed, result, sizeof(result), uri, sizeof(uri));
    assert(strcmp(result, "503 (1:223)") == 0);
    assert(strcmp(uri, "ims.telekom.de") == 0);

    volte_settings_parse_last_call(connected, result, sizeof(result), uri, sizeof(uri));
    assert(strcmp(result, "connected") == 0);
    assert(strcmp(uri, "tel:") == 0);

    /* The reg-event SUBSCRIBE after a registration is not a call. */
    const char *subscribe_only =
        "sip.ua.client: SubscribeToRegInfo(): uri sip:+391234@ims.mnc053.mcc222.3gppnetwork.org\n"
        "sip.stack.dialogmanager: createOutDialog(): R-URI: sip:+391234@ims.mnc053.mcc222.3gppnetwork.org\n";
    volte_settings_parse_last_call(subscribe_only, result, sizeof(result), uri, sizeof(uri));
    assert(strcmp(result, "no call seen") == 0);

    char after[512];
    snprintf(after, sizeof(after), "%s%s", connected, subscribe_only);
    volte_settings_parse_last_call(after, result, sizeof(result), uri, sizeof(uri));
    assert(strcmp(result, "connected") == 0);
    assert(strcmp(uri, "tel:") == 0);

    volte_settings_parse_last_call("", result, sizeof(result), uri, sizeof(uri));
    assert(strcmp(result, "no call seen") == 0);
    assert(uri[0] == '\0');
}

static void test_zip(void)
{
    char dir[] = "/tmp/volte-zip-XXXXXX";
    char zip[] = "/tmp/volte-zip-test.zip";
    unsigned char buf[4096];
    FILE *f;
    size_t n;

    assert(mkdtemp(dir) != NULL);
    char path[512];
    snprintf(path, sizeof(path), "%s/a.txt", dir);
    f = fopen(path, "w");
    assert(f && fputs("hello", f) >= 0);
    fclose(f);
    snprintf(path, sizeof(path), "%s/b.txt", dir);
    f = fopen(path, "w");
    assert(f && fputs("world", f) >= 0);
    fclose(f);

    unlink(zip);
    assert(zip_directory(dir, zip) == 0);

    f = fopen(zip, "rb");
    assert(f);
    n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    assert(n > 22);
    assert(memcmp(buf, "PK\x03\x04", 4) == 0);

    /* End of central directory: count = 2. */
    for (size_t i = n; i >= 22; i--)
    {
        if (memcmp(buf + i - 22, "PK\x05\x06", 4) == 0)
        {
            assert(buf[i - 22 + 10] == 2 && buf[i - 22 + 11] == 0);
            break;
        }
        if (i == 22)
            assert(0);
    }

    unlink(zip);
    snprintf(path, sizeof(path), "%s/a.txt", dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/b.txt", dir);
    unlink(path);
    rmdir(dir);
}

int main(void)
{
    test_request();
    test_learned_domain();
    test_last_call();
    test_zip();
    printf("test_volte_settings: OK\n");
    return 0;
}
