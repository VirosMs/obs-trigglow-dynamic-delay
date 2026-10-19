/*
Trigglow Dynamic Delay for OBS
Copyright (C) 2026 Trigglow (VirosMs)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

// Tests for src/log-redact.*: what is hidden in OBS's log before it is
// attached to a support ticket. Every name, address and token below is
// invented; see test-support.hpp for why some are assembled from parts.

#include "log-redact.hpp"
#include "test-support.hpp"

#include <chrono>
#include <string>

using testsupport::JoinPath;
using testsupport::Url;
using trigglow::logredact::RedactLine;
using trigglow::logredact::RedactText;

namespace {

void TestContractVectors()
{
	testcheck::g_currentTest = "contract vectors";
	// The timestamp stays, the Windows user name goes.
	const std::string accent = "P\xC3\xA9rez";
	CHECK_STR(
		RedactLine("21:04:11.532: settings saved to " +
			   JoinPath({"C:", "Users", "Carlos " + accent, "AppData", "Roaming", "obs-studio", "x.json"})),
		"21:04:11.532: settings saved to " +
			JoinPath({"C:", "Users", "<user>", "AppData", "Roaming", "obs-studio", "x.json"}));
	CHECK_STR(RedactLine("rtmp://live.example.com/app/live_123456789_abcdefGHIJ"), "<stream-url>");
	CHECK_STR(RedactLine(Url("https", "www.trigglow.com/overlay/AbCd1234XyZ?x=1")), "<url:www.trigglow.com>");
	CHECK_STR(RedactLine("source: " + Url("https", "user:pw@example.com/a")), "source: <url:example.com>");
	CHECK_STR(RedactLine("contact me at a.b@example.org"), "contact me at <email>");
	CHECK_STR(RedactLine("peer 192.168.1.20 ok"), "peer <ip> ok");
	CHECK_STR(RedactLine("OBS 31.0.2"), "OBS 31.0.2");
	CHECK_STR(RedactLine("Windows Version: 10.0 Build 22631"), "Windows Version: 10.0 Build 22631");
	CHECK_STR(RedactLine("token=abc123def456"), "token=<redacted>");
	CHECK_STR(RedactLine("Hotkey: Ctrl+F5"), "Hotkey: Ctrl+F5");
	CHECK_STR(RedactLine("obs-trigglow-dynamic-delay"), "obs-trigglow-dynamic-delay");
	CHECK_STR(RedactLine("a1b2c3d4e5f6a7b8c9d0e1f2a3b4"), "<token>");
}

void TestUserPaths()
{
	testcheck::g_currentTest = "user paths";
	// Windows, any drive, forward slashes, JSON-escaped backslashes, the old
	// profile folder, and the folder name in any case.
	CHECK_STR(RedactLine(JoinPath({"D:", "Users", "jane.doe", "Videos"})),
		  JoinPath({"D:", "Users", "<user>", "Videos"}));
	CHECK_STR(RedactLine("file:///" + JoinPath({"C:", "Users", "Ana", "x.png"}, "/")),
		  "file:///" + JoinPath({"C:", "Users", "<user>", "x.png"}, "/"));
	CHECK_STR(RedactLine("\"path\":\"" + JoinPath({"C:", "Users", "bob", "obs"}, "\\\\") + "\""),
		  "\"path\":\"" + JoinPath({"C:", "Users", "<user>", "obs"}, "\\\\") + "\"");
	CHECK_STR(RedactLine("C:\\Documents and Settings\\bob\\Desktop"),
		  "C:\\Documents and Settings\\<user>\\Desktop");
	CHECK_STR(RedactLine(JoinPath({"c:", "USERS", "Bob", "x"})), JoinPath({"c:", "USERS", "<user>", "x"}));
	// The name ends at the next separator or a character a name can't hold.
	CHECK_STR(RedactLine("'" + JoinPath({"C:", "Users", "zoe"}) + "'"),
		  "'" + JoinPath({"C:", "Users", "<user>"}) + "'");
	// Not a profile folder.
	CHECK_STR(RedactLine("C:\\Program Files\\obs-studio\\bin"), "C:\\Program Files\\obs-studio\\bin");
	CHECK_STR(RedactLine(JoinPath({"C:", "Users"}) + "\\"), JoinPath({"C:", "Users"}) + "\\");

	// macOS and Linux homes.
	CHECK_STR(RedactLine("/Users/jane.doe/Library/Application Support/obs-studio/logs"),
		  "/Users/<user>/Library/Application Support/obs-studio/logs");
	CHECK_STR(RedactLine(JoinPath({"", "home", "alice", ".config", "obs-studio"}, "/")),
		  JoinPath({"", "home", "<user>", ".config", "obs-studio"}, "/"));
	CHECK_STR(RedactLine("/home/dev_01"), "/home/<user>");
	CHECK_STR(RedactLine("/home/"), "/home/");
	// Case-sensitive, as on those systems.
	CHECK_STR(RedactLine("/HOME/x"), "/HOME/x");
}

void TestUrls()
{
	testcheck::g_currentTest = "urls";
	// Only the host stays: path, query (with its tokens), port and credentials go.
	CHECK_STR(RedactLine("GET " + Url("https", "example.com/path?token=abc&x=1") + " done"),
		  "GET <url:example.com> done");
	CHECK_STR(RedactLine(Url("http", "Example.COM:8080/stats")), "<url:Example.COM>");
	CHECK_STR(RedactLine("[browser] (source: " + Url("https", "www.trigglow.com/overlay/AbCdEf123456") + ")"),
		  "[browser] (source: <url:www.trigglow.com>"); // The path runs to the next space or quote.
	CHECK_STR(RedactLine("\"" + Url("https", "cdn.example.net/a b") + "\""), "\"<url:cdn.example.net> b\"");
	CHECK_STR(RedactLine(Url("HTTPS", "example.com#frag")), "<url:example.com>");
	// An IP host becomes <ip> too, through the IPv4 rule.
	CHECK_STR(RedactLine(Url("http", "192.168.1.10:8080/x")), "<url:<ip>>");
	// Not a URL: no host. Any scheme counts, even one glued to a word ("xhttp").
	CHECK_STR(RedactLine("https:// nothing"), "https:// nothing");
	CHECK_STR(RedactLine("x" + Url("http", "example.com")), "<url:example.com>");

	// Stream URLs disappear whole (the path holds the stream key).
	CHECK_STR(RedactLine("server rtmps://a.rtmp.example.com:443/app key"), "server <stream-url> key");
	CHECK_STR(RedactLine("srt://1.2.3.4:9000?streamid=abc"), "<stream-url>");
	CHECK_STR(RedactLine("RIST://host/x"), "<stream-url>");
	CHECK_STR(RedactLine("whip://host/x rtsp://cam/1"), "<stream-url> <stream-url>");
	CHECK_STR(RedactLine("rtmp:// alone"), "rtmp:// alone");

	// Overlay links outside a URL.
	CHECK_STR(RedactLine("opened /overlay/AbCd_12-34 ok"), "opened /overlay/<slug> ok");
}

void TestEmails()
{
	testcheck::g_currentTest = "emails";
	CHECK_STR(RedactLine("from: first.last+tag@mail.example.co.uk."), "from: <email>.");
	CHECK_STR(RedactLine("x@y"), "x@y");
	CHECK_STR(RedactLine("@user mention"), "@user mention");
	CHECK_STR(RedactLine("a@b@example.com"), "a@<email>");
	CHECK_STR(RedactLine("host.c@1.2"), "host.c@1.2");
	CHECK_STR(RedactLine("name@example.com2x"), "<email>2x");
}

void TestSecrets()
{
	testcheck::g_currentTest = "secrets";
	CHECK_STR(RedactLine("stream_key=live_abc123"), "stream_key=<redacted>");
	CHECK_STR(RedactLine("streamkey: xyz"), "streamkey=<redacted>");
	CHECK_STR(RedactLine("Password = hunter2, next"), "Password=<redacted>, next");
	CHECK_STR(RedactLine("?access_token=abc&sig=ff00"), "?access_token=<redacted>&sig=<redacted>");
	CHECK_STR(RedactLine("signature:abcdef"), "signature=<redacted>");
	CHECK_STR(RedactLine("API_KEY=k1; apikey=k2"), "API_KEY=<redacted>; apikey=<redacted>");
	CHECK_STR(RedactLine("refresh_token = r1"), "refresh_token=<redacted>");
	// Quoted values keep their quotes.
	CHECK_STR(RedactLine("passphrase:'x'"), "passphrase:'<redacted>'");
	// The key word must end the name as a whole part of it, followed by = or :.
	CHECK_STR(RedactLine("tokens=3"), "tokens=3");
	CHECK_STR(RedactLine("my_token=3"), "my_token=<redacted>");
	CHECK_STR(RedactLine("authorization pending"), "authorization pending");

	// Bearer tokens, plugin tokens and JWTs.
	const std::string pluginTok = "tgp_0123456789abcdefghijABCDEFGHIJ_-klmnopqrstu";
	CHECK_STR(RedactLine("Authorization: Bearer " + pluginTok), "Authorization: Bearer <redacted>");
	CHECK_STR(RedactLine("bearer   " + pluginTok + "; x"), "bearer <redacted>; x");
	CHECK_STR(RedactLine("got " + pluginTok), "got <token>");
	const std::string jwt = std::string("ey") + "JhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9" + "." +
				"eyJzdWIiOiIxMjM0NTY3ODkwIiwiaWF0IjoxNTE2MjM5MDIyfQ" + "." +
				"SflKxwRJSMeKKF2QT4fwpMeJf36POk6yJV_adQssw5c";
	CHECK_STR(RedactLine("jwt " + jwt), "jwt <token>");
}

void TestAddresses()
{
	testcheck::g_currentTest = "ip addresses";
	CHECK_STR(RedactLine("10.0.0.1:443"), "<ip>:443");
	CHECK_STR(RedactLine("from 255.255.255.255."), "from <ip>.");
	CHECK_STR(RedactLine("256.1.1.1"), "256.1.1.1");
	CHECK_STR(RedactLine("1.2.3"), "1.2.3");
	CHECK_STR(RedactLine("10.0.22631.4317"), "10.0.22631.4317"); // A Windows build number.
	CHECK_STR(RedactLine("v1.2.3.4a"), "v1.2.3.4a");
	CHECK_STR(RedactLine("1.2.3.4.5"), "<ip>.5");
	CHECK_STR(RedactLine("010.1.1.1"), "<ip>"); // Leading zeros: still an address.
	CHECK_STR(RedactLine("CPU Speed: 3.60GHz, 12.5 fps"), "CPU Speed: 3.60GHz, 12.5 fps");

	CHECK_STR(RedactLine("addr 2001:0db8:85a3:0000:0000:8a2e:0370:7334 up"), "addr <ip> up");
	CHECK_STR(RedactLine("fe80::1ff:fe23:4567:890a%12"), "<ip>%12");
	CHECK_STR(RedactLine("[::1]:8080"), "[<ip>]:8080");
	CHECK_STR(RedactLine("mac aa:bb:cc:dd:ee:ff"), "mac <ip>");
	CHECK_STR(RedactLine("21:04:11.532: started"), "21:04:11.532: started");
	CHECK_STR(RedactLine("std::string and Info :: x"), "std::string and Info :: x");
	CHECK_STR(RedactLine("12:30:45"), "12:30:45");
}

void TestLongTokens()
{
	testcheck::g_currentTest = "long tokens";
	CHECK_STR(RedactLine("{4D36E968-E325-11CE-BFC1-08002BE10318}"), "{<token>}");
	CHECK_STR(RedactLine("abcdefghijklmnopqrstuvwxyz"), "abcdefghijklmnopqrstuvwxyz");         // No digit.
	CHECK_STR(RedactLine("123456789012345678901234567890"), "123456789012345678901234567890"); // No letter.
	CHECK_STR(RedactLine("a1b2c3d4e5f6a7b8c9d0e1f"), "a1b2c3d4e5f6a7b8c9d0e1f");               // 23 characters.
	CHECK_STR(RedactLine("win-capture-hook-x64-2026"), "<token>");
}

void TestWholeText()
{
	testcheck::g_currentTest = "whole text";
	const std::string in = "line one 10.1.2.3\r\n" + JoinPath({"C:", "Users", "bob"}) + "\n\nlast token=x";
	const std::string out = "line one <ip>\r\n" + JoinPath({"C:", "Users", "<user>"}) + "\n\nlast token=<redacted>";
	CHECK_STR(RedactText(in), out);
	CHECK_STR(RedactText(""), "");
	CHECK_STR(RedactText("\n"), "\n");
	CHECK_STR(RedactText("no newline at end"), "no newline at end");
	// Idempotent.
	CHECK_STR(RedactText(out), out);
}

void TestCap()
{
	testcheck::g_currentTest = "upload cap";
	using trigglow::logredact::CapForUpload;
	using trigglow::logredact::kUploadCapBytes;
	bool capped = true;
	CHECK_STR(CapForUpload("small", &capped), "small");
	CHECK(!capped);

	std::string big;
	int n = 0;
	while (big.size() < kUploadCapBytes + 1000000)
		big += "line " + std::to_string(n++) + " \xC3\xA9\xC3\xA9 some text\n";
	const std::string out = CapForUpload(big, &capped);
	CHECK(capped);
	CHECK(out.size() <= kUploadCapBytes);
	CHECK(out.size() > kUploadCapBytes - 200);
	const size_t marker = out.find(trigglow::logredact::kCutMarker);
	CHECK(marker != std::string::npos);
	CHECK(marker <= trigglow::logredact::kUploadHeadBytes);
	CHECK(out.compare(0, 7, "line 0 ") == 0);
	CHECK(out[marker - 1] == '\n'); // The head ends with a whole line.
	const size_t tail = marker + std::string(trigglow::logredact::kCutMarker).size();
	CHECK(out.compare(tail, 5, "line ") == 0); // The tail starts with a whole line.
	CHECK(out.compare(out.size() - 10, 10, big.substr(big.size() - 10)) == 0);

	// One giant line: still capped, and never cut inside a UTF-8 sequence.
	std::string line(kUploadCapBytes + 10, 'a');
	for (size_t i = 0; i + 1 < line.size(); i += 3) {
		line[i] = '\xC3';
		line[i + 1] = '\xA9';
	}
	const std::string cut = CapForUpload(line, &capped);
	CHECK(cut.size() <= kUploadCapBytes);
	const size_t m = cut.find(trigglow::logredact::kCutMarker);
	CHECK(m != std::string::npos);
	const size_t t = m + std::string(trigglow::logredact::kCutMarker).size();
	CHECK(t < cut.size() && (static_cast<unsigned char>(cut[t]) & 0xC0) != 0x80);
	CHECK(m > 0 && (static_cast<unsigned char>(cut[m - 1]) & 0xE0) != 0xC0);
}

void TestPreview()
{
	testcheck::g_currentTest = "preview";
	using trigglow::logredact::MakePreview;
	std::string text;
	for (int i = 0; i < 1000; ++i)
		text += "L" + std::to_string(i) + "\n";
	const auto p = MakePreview(text, 200);
	CHECK(p.truncated);
	CHECK(p.totalLines == 1000);
	CHECK(p.head.rfind("L0\n", 0) == 0);
	CHECK(p.head.size() >= 4 && p.head.compare(p.head.size() - 5, 5, "L199\n") == 0);
	CHECK(p.tail.rfind("L800\n", 0) == 0);
	CHECK(p.tail.compare(p.tail.size() - 5, 5, "L999\n") == 0);
	const auto small = MakePreview("a\nb", 200);
	CHECK(!small.truncated);
	CHECK_STR(small.head, "a\nb");
	CHECK(small.tail.empty());
	CHECK(small.totalLines == 2);
}

void TestRobustness()
{
	testcheck::g_currentTest = "robustness";
	// A 2 MB line of URL-ish, path-ish and address-ish characters: linear
	// scanners, no recursion, so no stack overflow.
	std::string line;
	while (line.size() < 2 * 1024 * 1024)
		line += Url("https", "a.b/c?d=1&token=2") + " 1.2.3.4 aa:bb::cc " + JoinPath({"C:", "Users", "x"}) +
			" a@b.cc ";
	const auto start = std::chrono::steady_clock::now();
	const std::string out = RedactLine(line);
	const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	CHECK(out.find("token=2") == std::string::npos);
	CHECK(out.find("1.2.3.4") == std::string::npos);

	// A 5 MB log of ordinary lines.
	std::string log;
	int i = 0;
	while (log.size() < 5 * 1024 * 1024)
		log += "12:00:00.000: [obs-trigglow-dynamic-delay] [buffer-mode] frame " + std::to_string(i++) +
		       " ok, render 1.23 ms, skipped 0\n";
	const auto start2 = std::chrono::steady_clock::now();
	const std::string redacted = RedactText(log);
	const double ms2 = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start2).count();
	CHECK(redacted == log);
	std::printf("  2 MB hostile line: %.0f ms; 5 MB log: %.0f ms\n", ms, ms2);
	CHECK(ms2 < 20000.0);

	// Invalid UTF-8 and NULs pass through untouched.
	const std::string odd("a\xFF\xFE\0b", 5);
	CHECK(RedactLine(odd) == odd);
}

// `secret` is nowhere in the redacted line (prints the line when it is).
bool Hidden(const std::string &line, const std::string &secret)
{
	const std::string out = RedactLine(line);
	const bool hidden = out.find(secret) == std::string::npos;
	if (!hidden)
		std::printf("  still there: \"%s\" in \"%s\"\n", secret.c_str(), out.c_str());
	return hidden;
}

// The 0.6.0 security review's cases, with invented data: each one got through
// an earlier version of the rules.
void TestReviewPaths()
{
	testcheck::g_currentTest = "review: paths";
	CHECK(Hidden("settings saved to " + JoinPath({"C:", "Users", "Carlos Perez", "AppData", "x.json"}), "Carlos"));
	CHECK_STR(RedactLine("loaded " + JoinPath({"C:", "Users", "Sean O'Brien", "Videos", "a.mp4"})),
		  "loaded " + JoinPath({"C:", "Users", "<user>", "Videos", "a.mp4"}));
	CHECK_STR(RedactLine(JoinPath({"c:", "users", "alice", "obs", "x.png"}, "/")),
		  JoinPath({"c:", "users", "<user>", "obs", "x.png"}, "/"));
	CHECK(Hidden(JoinPath({"C:", "Users", "CARLOS~1", "AppData"}), "CARLOS"));
	CHECK_STR(RedactLine("\\\\?\\" + JoinPath({"C:", "Users", "dana", "x"})),
		  "\\\\?\\" + JoinPath({"C:", "Users", "<user>", "x"}));
	// A network share: the computer's name goes too.
	CHECK_STR(RedactLine(JoinPath({"\\\\DESKTOP-ANA", "c$", "Users", "ana", "x.png"})),
		  JoinPath({"\\\\<host>", "c$", "Users", "<user>", "x.png"}));
	CHECK_STR(RedactLine("open " + JoinPath({"\\\\NAS-ANA", "videos", "intro.mp4"})),
		  "open " + JoinPath({"\\\\<host>", "videos", "intro.mp4"}));
	CHECK_STR(RedactLine("file:///C%3A/" + JoinPath({"Users", "frank", "a.png"}, "/")),
		  "file:///C%3A/" + JoinPath({"Users", "<user>", "a.png"}, "/"));
	// A profile moved to another folder, and OneDrive for work.
	CHECK_STR(RedactLine(JoinPath({"D:", "Profiles", "gina", "Documents", "a.png"})),
		  JoinPath({"D:", "Profiles", "<user>", "Documents", "a.png"}));
	CHECK_STR(RedactLine(JoinPath({"E:", "Home", "gina", "Saved Games", "x"}, "/")),
		  JoinPath({"E:", "Home", "<user>", "Saved Games", "x"}, "/"));
	CHECK_STR(RedactLine(JoinPath({"C:", "Users", "hugo", "OneDrive - Acme Corp", "a.png"})),
		  JoinPath({"C:", "Users", "<user>", "OneDrive - <org>", "a.png"}));
	CHECK_STR(RedactLine(JoinPath({"C:", "Users", "hugo", "OneDrive", "Documents", "a.png"})),
		  JoinPath({"C:", "Users", "<user>", "OneDrive", "Documents", "a.png"}));
	// macOS ignores case; other places for a home; MSYS paths.
	CHECK_STR(RedactLine(JoinPath({"", "users", "judy", "Movies", "a.mov"}, "/")),
		  JoinPath({"", "users", "<user>", "Movies", "a.mov"}, "/"));
	CHECK_STR(RedactLine(JoinPath({"", "Volumes", "Data", "judy", "Movies", "a.mov"}, "/")),
		  JoinPath({"", "Volumes", "Data", "<user>", "Movies", "a.mov"}, "/"));
	CHECK_STR(RedactLine(JoinPath({"", "var", "home", "leo", ".config", "x"}, "/")),
		  JoinPath({"", "var", "home", "<user>", ".config", "x"}, "/"));
	CHECK_STR(RedactLine(JoinPath({"", "c", "Users", "mia", "x"}, "/")),
		  JoinPath({"", "c", "Users", "<user>", "x"}, "/"));

	// What must stay: OBS's own folders, Windows' device paths, JSON-escaped
	// program paths, and folders that only look like a profile's.
	for (const std::string &kept :
	     {JoinPath({"C:", "Program Files", "obs-studio", "data", "obs-plugins", "frontend-tools", "locale",
			"en-US.ini"},
		       "/"),
	      std::string("\\\\.\\DISPLAY1"), std::string("C:\\\\Program Files\\\\obs-studio\\\\bin"),
	      JoinPath({"D:", "Videos", "intro.mp4"}), JoinPath({"D:", "Documents", "Videos", "x"}),
	      std::string("[win-wasapi: 'Mic/Aux'] update settings:"),
	      JoinPath({"", "usr", "lib", "x86_64-linux-gnu", "obs-plugins"}, "/")}) {
		CHECK_STR(RedactLine(kept), kept);
	}
}

void TestReviewUrls()
{
	testcheck::g_currentTest = "review: urls";
	CHECK_STR(RedactLine("source: " + Url("https", "x.example.com/a?token=SeCrEt1")),
		  "source: <url:x.example.com>");
	// The credentials end at the authority's LAST '@'.
	CHECK_STR(RedactLine(Url("https", "user:p@ss@evil.example/priv?k=v")), "<url:evil.example>");
	CHECK_STR(RedactLine(Url("https", "user@/x")), "<url>");
	// Any scheme keeps only its host.
	CHECK_STR(RedactLine(Url("wss", "pubsub.example.com/ws/abc123SECRET")), "<url:pubsub.example.com>");
	CHECK_STR(RedactLine(Url("ws", "192.0.2.10:4455/?password=abc")), "<url:<ip>>");
	CHECK_STR(RedactLine(Url("ftp", "user:pw123@files.example.com/x")), "<url:files.example.com>");
	CHECK_STR(RedactLine(Url("udp", "203.0.113.9:5000?pkt_size=1316")), "<url:<ip>>");
	CHECK_STR(RedactLine(Url("obsws", "192.0.2.10:4455/s3cret")), "<url:<ip>>");
	CHECK_STR(RedactLine(Url("http", "[2001:db8::1]:8080/x")), "<url:[<ip>]>");
	CHECK_STR(RedactLine("C://x"), "C://x"); // A drive, not a scheme.
	// Every RTMP and RTSP flavor is a stream URL.
	for (const char *scheme : {"rtmpt", "rtmpe", "rtmpte", "rtmps", "rtsps", "whep"})
		CHECK_STR(RedactLine(Url(scheme, "live.example.com/app/live_123_key")), "<stream-url>");
	// JSON-escaped slashes.
	CHECK_STR(RedactLine("\"https:\\/\\/www.trigglow.com\\/overlay\\/AbC123xyz?x=1\""),
		  "\"<url:www.trigglow.com>\"");
	CHECK_STR(RedactLine("\"rtmp:\\/\\/live.example.com\\/app\\/key1\""), "\"<stream-url>\"");
	CHECK_STR(RedactLine("\"\\/overlay\\/AbC123xyz\""), "\"\\/overlay\\/<slug>\"");
	CHECK_STR(RedactLine("www.trigglow.com/overlay/Zz9_slug"), "www.trigglow.com/overlay/<slug>");
}

void TestReviewSecrets()
{
	testcheck::g_currentTest = "review: secrets";
	// Stream keys without a digit, and keys after any name ending in a key word.
	CHECK_STR(RedactLine("abcd-efgh-ijkl-mnop-qrst"), "<token>");
	CHECK_STR(RedactLine("server cfg key=Short123"), "server cfg key=<redacted>");
	CHECK_STR(RedactLine("client_secret=abc123def"), "client_secret=<redacted>");
	CHECK_STR(RedactLine("x_api_key=abc123def"), "x_api_key=<redacted>");
	CHECK_STR(RedactLine("X-Api-Key: abc123"), "X-Api-Key=<redacted>");
	CHECK_STR(RedactLine("server_passphrase=abc"), "server_passphrase=<redacted>");
	CHECK_STR(RedactLine("oauth:abcdefghijklmnop"), "oauth=<redacted>");
	CHECK_STR(RedactLine("refreshToken=r1 db_pwd=p2"), "refreshToken=<redacted> db_pwd=<redacted>");
	// Credential headers: the rest of the line goes.
	CHECK_STR(RedactLine("Authorization: Basic dXNlcjpwYXNz"), "Authorization: Basic <redacted>");
	CHECK_STR(RedactLine("Authorization: Token 9f8e7d"), "Authorization: Token <redacted>");
	CHECK_STR(RedactLine("Authorization: 9f8e7d"), "Authorization: <redacted>");
	CHECK_STR(RedactLine("Proxy-Authorization: Digest username=\"ana\", realm=\"x\""),
		  "Proxy-Authorization: Digest <redacted>");
	CHECK_STR(RedactLine("Cookie: session=abcdef123; theme=dark"), "Cookie: <redacted>");
	CHECK_STR(RedactLine("Set-Cookie: sid=abc; Path=/; HttpOnly"), "Set-Cookie: <redacted>");
	// JSON and quoted values keep their quotes.
	CHECK_STR(RedactLine(R"({"token":"tok_9x8y7z"})"), R"({"token":"<redacted>"})");
	CHECK_STR(RedactLine(R"({"password": "hunter2"})"), R"({"password": "<redacted>"})");
	CHECK_STR(RedactLine(R"({"apiKey":"k1234567"})"), R"({"apiKey":"<redacted>"})");
	// (Not raw literals: MSVC can't stringize one with a backslash in CHECK_STR.)
	const std::string escapedQuote = "{\"a\":1,\"stream_key\":\"a\\\"b\",\"c\":2}";
	CHECK_STR(RedactLine(escapedQuote), "{\"a\":1,\"stream_key\":\"<redacted>\",\"c\":2}");
	const std::string jsonInString = "msg {\\\"access_token\\\":\\\"abc\\\"}";
	CHECK_STR(RedactLine(jsonInString), "msg {\\\"access_token\\\":\\\"<redacted>\\\"}");
	CHECK_STR(RedactLine(R"({"refresh_token": null})"), R"({"refresh_token": <redacted>})");
	CHECK_STR(RedactLine(R"({"Authorization":"Basic dXNl"})"), R"({"Authorization":"<redacted>"})");
	CHECK_STR(RedactLine("password=\"a b c\" next"), "password=\"<redacted>\" next");
	CHECK_STR(RedactLine("'stream_key'='k1'"), "'stream_key'='<redacted>'");
	CHECK_STR(RedactLine("token=\"never closed"), "token=\"<redacted>");
	CHECK_STR(RedactLine(R"({"token":""})"), R"({"token":""})"); // Nothing to hide.

	// JWTs whole, also with a short or empty part.
	const std::string header = std::string("ey") + "JhbGciOiJIUzI1NiJ9";
	const std::string payload = std::string("ey") + "JzdWIiOiIxIn0";
	CHECK_STR(RedactLine("jwt " + header + "." + payload + ".c2lnbmF0dXJlMTIzNDU2Nzg5MA"), "jwt <token>");
	CHECK_STR(RedactLine(std::string("ey") + "JhbGciOiJub25lIn0" + "." + std::string("ey") + "JhIjoxfQ" + "."),
		  "<token>");
	// Standard base64, with '+' and '/'.
	CHECK_STR(RedactLine("secretblob Ab3d+Ef9/Gh12Ij34Kl56Mn78Op90"), "secretblob <token>");
	CHECK_STR(RedactLine("x=Zm9vYmFyQmF6/Uu8xYz09+Q3wPa1=="), "x=<token>");

	// What must stay.
	for (const std::string &kept :
	     {std::string("Hotkey: Ctrl+F5"), std::string("monkey=3"), std::string("keyint: 250"),
	      std::string("keyframes: 2"), std::string("tokens=3"), std::string("authorization pending"),
	      std::string("this-that-with-some-words"), std::string("render 1.23 ms/frame, 60/60 fps"),
	      JoinPath({"", "opt", "data", "MyApp2", "Config", "Version3", "x"}, "/"),
	      std::string("Applications/OBS.app/Contents/Frameworks/QtCore.framework/Versions/A/QtCore")}) {
		CHECK_STR(RedactLine(kept), kept);
	}
}

void TestReviewPeople()
{
	testcheck::g_currentTest = "review: emails, addresses, devices";
	CHECK_STR(RedactLine("contact=bob%40example.com"), "contact=<email>");
	CHECK_STR(RedactLine("contact=bob%40example.com&x=1"), "contact=<email>&x=1");
	CHECK_STR(RedactLine("mail jos\xC3\xA9@example.org"), "mail <email>");
	CHECK_STR(RedactLine(std::string("mail bob@correo.espa\xC3\xB1") + "a"), "mail <email>");
	CHECK_STR(RedactLine("mail o'brien@example.org"), "mail <email>");
	CHECK_STR(RedactLine("'bob@example.org'"), "'<email>'");
	CHECK_STR(RedactLine("it's bob@example.org"), "it's <email>");
	CHECK_STR(RedactLine("a%40b"), "a%40b");

	CHECK_STR(RedactLine("peer 010.001.002.003"), "peer <ip>");
	CHECK_STR(RedactLine("999.1.1.1"), "999.1.1.1");
	CHECK_STR(RedactLine("1.2.3.0004"), "1.2.3.0004");

	// Audio devices and windows, where the name is quoted or ends the line.
	CHECK_STR(RedactLine("[wasapi] Device 'Headphones (Ana's AirPods)' init"), "[wasapi] Device '<device>' init");
	CHECK_STR(RedactLine("WASAPI: Device 'Mic (Ana)' [48000 Hz] initialized"),
		  "WASAPI: Device '<device>' [48000 Hz] initialized");
	CHECK_STR(RedactLine("\tvideo device: Cam of Ana"), "\tvideo device: <device>");
	CHECK_STR(RedactLine("device name: \"Ana's iPhone\""), "device name: \"<device>\"");
	CHECK_STR(RedactLine("[window-capture] capturing 'Chat with Ana Lopez - Discord'"),
		  "[window-capture] capturing '<window>'");
	CHECK_STR(RedactLine("window \"Inbox - ana\" found"), "window \"<window>\" found");
	CHECK_STR(RedactLine("\ttitle: Ana's notes - Editor"), "\ttitle: <window>");
	for (const std::string &kept :
	     {std::string("use device timing: 1"), std::string("Audio monitoring device:"),
	      std::string("[window-capture: 'Window Capture'] update settings:"), std::string("Device Manager"),
	      std::string("devices: 2"), std::string("subtitle: x"), std::string("window: 1920x1080")}) {
		CHECK_STR(RedactLine(kept), kept);
	}
}

std::string Repeated(const std::string &piece, size_t bytes)
{
	std::string s;
	while (s.size() < bytes)
		s += piece;
	return s;
}

// The scanners stay linear on lines built to slow down a naive one.
void TestHostileLines()
{
	testcheck::g_currentTest = "hostile lines";
	const size_t mb = 1 << 20;
	const std::string lines[] = {
		std::string(mb, '\\'),
		std::string(mb, '/'),
		std::string(mb, '@'),
		std::string(mb, '\''),
		Repeated("device 'a token=\"b " + std::string("ey") + "Jxx.yy Ab3d+Ef9/ \\\\h\\ ", mb),
		Repeated("a://!x%40y.z%40 " + JoinPath({"C:", "Users", "a'b'c'd"}) + " /Users/ ", mb),
	};
	for (const std::string &line : lines) {
		const auto start = std::chrono::steady_clock::now();
		const std::string out = RedactLine(line);
		const double ms =
			std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		CHECK(!out.empty());
		CHECK(ms < 10000.0);
	}
}

} // namespace

int main()
{
	TestContractVectors();
	TestUserPaths();
	TestUrls();
	TestEmails();
	TestSecrets();
	TestAddresses();
	TestLongTokens();
	TestWholeText();
	TestCap();
	TestPreview();
	TestRobustness();
	TestReviewPaths();
	TestReviewUrls();
	TestReviewSecrets();
	TestReviewPeople();
	TestHostileLines();
	return testcheck::Finish("log_redact_tests");
}
