#!/usr/bin/env python3
"""Generate blocklist.json, a Safari content-blocker ruleset for WebKit.

Not a general EasyList converter. A full EasyList compile is ~40k rules, and on
a 1 GHz A8 that is a multi-second startup cost every time the list changes, for
a long tail of rules that a phone browser will never match. This is the head of
the distribution instead: the third-party ad and tracking hosts that account
for most of the requests on an ordinary page, taken from EasyList and
EasyPrivacy.

Every rule is `third-party` on purpose. First-party loads are left alone, so
actually visiting facebook.com or navigating to a tracker's own site still
works -- what is blocked is those hosts being pulled INTO someone else's page.

    python3 app/make-blocklist.py > app/blocklist.json
"""
import json
import sys

# Ad serving and exchanges.
ADS = """
doubleclick.net googlesyndication.com googleadservices.com adservice.google.com
adnxs.com rubiconproject.com pubmatic.com openx.net casalemedia.com
smartadserver.com adform.net criteo.com criteo.net teads.tv 33across.com
sharethrough.com indexww.com contextweb.com sovrn.com lijit.com gumgum.com
media.net yieldmo.com spotxchange.com stickyadstv.com innovid.com
flashtalking.com serving-sys.com sitescout.com simpli.fi districtm.io
improvedigital.com emxdgt.com mathtag.com bidswitch.net adroll.com
mgid.com revcontent.com plista.com zergnet.com outbrain.com taboola.com
adcolony.com applovin.com inmobi.com smaato.net mopub.com
googletagservices.com amazon-adsystem.com adsrvr.org id5-sync.com
"""

# Measurement, attribution and session replay.
TRACKERS = """
google-analytics.com googletagmanager.com scorecardresearch.com quantserve.com
chartbeat.com chartbeat.net parsely.com nr-data.net newrelic.com
segment.com segment.io mixpanel.com amplitude.com hotjar.com fullstory.com
mouseflow.com crazyegg.com optimizely.com branch.io appsflyer.com
adjust.com kochava.com bluekai.com krxd.net demdex.net omtrdc.net
everesttech.net adobedtm.com 2o7.net agkn.com rlcdn.com exelator.com
tapad.com moatads.com adsafeprotected.com doubleverify.com
bat.bing.com analytics.twitter.com ads-twitter.com analytics.tiktok.com
mc.yandex.ru addthis.com sharethis.com onesignal.com cloudflareinsights.com
connect.facebook.net
"""


def rules(domains):
    for d in domains:
        # ([^/]*\.)? so a bare host and any subdomain both match, and the
        # trailing [:/] keeps "evil-doubleclick.net.example.com" out of it by
        # requiring the host to END here.
        yield {
            "trigger": {
                "url-filter": r"^https?://([^/]*\.)?%s[:/]" % d.replace(".", r"\."),
                "load-type": ["third-party"],
            },
            "action": {"type": "block"},
        }


def main():
    domains = sorted(set(ADS.split() + TRACKERS.split()))
    out = list(rules(domains))
    json.dump(out, sys.stdout, separators=(",", ":"))
    sys.stdout.write("\n")
    print("%d domains, %d rules" % (len(domains), len(out)), file=sys.stderr)


if __name__ == "__main__":
    main()
