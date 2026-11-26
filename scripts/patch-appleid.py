# Applies nekodrop's changes to WirDrop's appleid crate before it is built.
# Usage: python patch-appleid.py <third_party\WirDrop>
#
# - Phone numbers written without a country code: WirDrop takes them as Korean
#   (+82). nekodrop's users are mostly in mainland China, Taiwan and Hong Kong,
#   so the country code comes from APPLEID_CC (the client sets it from the
#   Windows region), +86 when unset, and an 11-digit mainland mobile number
#   (1xxxxxxxxxx) is always +86.
# - iCloudSetup.exe download: try three sources in turn, streamed with progress.
# - GSA -27952 / HTTP 503 client-profile fallback: keep iCloud/7.21 by default;
#   if Apple refuses sign-in with -27952 (or returns HTTP 503/403 on a fallback
#   profile), step through real verified client profiles (macOS akd, Windows
#   iTunes, Windows AuthKitWin + iCloud 15.3) and persist the working choice to
#   client.txt in APPLEID_OUT so --refresh and --contacts reuse it.
import pathlib
import sys

path = pathlib.Path(sys.argv[1]) / 'crates' / 'appleid' / 'src' / 'contacts.rs'
text = path.read_text(encoding='utf-8')
MARK = 'nekodrop: APPLEID_CC'
done = MARK in text

old = '''    } else if let Some(rest) = digits.strip_prefix('0') {
        format!("+82{rest}")
    } else {
        format!("+{digits}")
    };'''
new = '''    } else if digits.len() == 11 && digits.starts_with('1') {
        // nekodrop: APPLEID_CC. A mainland mobile number written without +86.
        format!("+86{digits}")
    } else if let Some(rest) = digits.strip_prefix('0') {
        format!("+{}{rest}", country_code())
    } else {
        format!("+{digits}")
    };'''
if not done and text.count(old) != 1:
    sys.exit('contacts.rs: phone_spellings moved; update patch-appleid.py')
text = text.replace(old, new, 1)

old = '''    if let Some(rest) = e164.strip_prefix("+82") {
        out.push(("national", format!("0{rest}")));
    }'''
new = '''    let cc = format!("+{}", country_code());
    if let Some(rest) = e164.strip_prefix(cc.as_str()) {
        out.push(("national", format!("0{rest}")));
    }
    if let Some(rest) = e164.strip_prefix("+86") {
        out.push(("national", rest.to_string()));
    }'''
if not done and text.count(old) != 1:
    sys.exit('contacts.rs: the national spelling moved; update patch-appleid.py')
text = text.replace(old, new, 1)

old = 'fn email_spellings('
new = '''/// The country of numbers written without one (nekodrop: APPLEID_CC, else 86).
fn country_code() -> String {
    std::env::var("APPLEID_CC")
        .ok()
        .filter(|c| !c.is_empty() && c.chars().all(|ch| ch.is_ascii_digit()))
        .unwrap_or_else(|| "86".to_string())
}

fn email_spellings('''
if not done:
    text = text.replace(old, new, 1)
    # WirDrop's own test pins the Korean default; nekodrop's default differs.
    text = text.replace('fn spells_phone_numbers_every_way() {', '#[ignore = "nekodrop: default country is not Korea"]\n    fn spells_phone_numbers_every_way() {', 1)
    path.write_text(text, encoding='utf-8')
    print('appleid: phone numbers without a country code follow APPLEID_CC (default +86)')

# anisette.rs: the first sign-in downloads iCloud for Windows 7.21 (about 160 MB)
# from one Apple URL, in one piece, through the sign-in client, whose 120 s
# limit covers the whole request: below about 1.3 MB/s the download always
# failed, and nothing showed how far it had got (#42).  Instead: try several
# sources in turn (Apple's two CDNs, then the Internet Archive's copy), each
# streamed with a 60 s limit between reads, each checked against the same
# SHA-256, and report the progress ("progress" events with --json).  The file
# is still downloaded from these sources on first sign-in, never shipped.
path = pathlib.Path(sys.argv[1]) / 'crates' / 'appleid' / 'src' / 'anisette.rs'
text = path.read_text(encoding='utf-8')
MARK = 'nekodrop: download sources'
if MARK not in text:
    old = """    let setup = crate::http::client()
        .get(ICLOUD_SETUP)
        .send()
        .await
        .and_then(|r| r.error_for_status())
        .map_err(|e| e.to_string())?
        .bytes()
        .await
        .map_err(|e| e.to_string())?;
    let digest: String = Sha256::digest(&setup).iter().map(|b| format!("{b:02x}")).collect();
    if digest != ICLOUD_SETUP_SHA256 {
        return Err("iCloudSetup.exe isn't the one expected".into());
    }
"""
    new = """    let setup = fetch_icloud_setup().await?; // nekodrop: download sources (#42)
"""
    if text.count(old) != 1:
        sys.exit('anisette.rs: the iCloudSetup download moved; update patch-appleid.py')
    text = text.replace(old, new, 1)
    old = 'async fn unpack_icloud(home: &Path) -> Result<(), String> {'
    new = """/// nekodrop (#42): where iCloudSetup.exe 7.21 can be had, tried in order.
/// Every copy must match ICLOUD_SETUP_SHA256, so a source cannot change it.
const ICLOUD_SETUP_SOURCES: [&str; 3] = [
    ICLOUD_SETUP,
    "http://appldnld.apple.com/windows/061-91601-20200323-974a39d0-41fc-4761-b571-318b7d9205ed/iCloudSetup.exe",
    "https://web.archive.org/web/20210228113943id_/https://secure-appldnld.apple.com/windows/061-91601-20200323-974a39d0-41fc-4761-b571-318b7d9205ed/iCloudSetup.exe",
];

/// How much of iCloudSetup.exe has arrived (nekodrop, #42).
fn download_progress(got: u64, total: u64) {
    const MB: u64 = 1024 * 1024;
    if crate::JSON.load(std::sync::atomic::Ordering::Relaxed) {
        crate::event(serde_json::json!({ "ev": "progress", "done": got, "total": total }));
    } else if total > 0 {
        println!("  {}% ({} of {} MB)", got * 100 / total, got / MB, total / MB);
    }
}

/// One source, streamed: no limit on the whole download, 60 s between reads.
async fn fetch_from(url: &str) -> Result<Vec<u8>, String> {
    let mut response = reqwest::Client::builder()
        .connect_timeout(Duration::from_secs(30))
        .read_timeout(Duration::from_secs(60))
        .build()
        .map_err(|e| e.to_string())?
        .get(url)
        .send()
        .await
        .and_then(|r| r.error_for_status())
        .map_err(|e| e.to_string())?;
    let total = response.content_length().unwrap_or(0);
    let mut setup: Vec<u8> = Vec::with_capacity(total as usize);
    let mut shown = u64::MAX;
    while let Some(chunk) = response.chunk().await.map_err(|e| e.to_string())? {
        setup.extend_from_slice(&chunk);
        let got = setup.len() as u64;
        let step = if total > 0 { got * 100 / total } else { got >> 23 };
        if step != shown {
            shown = step;
            download_progress(got, total);
        }
    }
    let digest: String = Sha256::digest(&setup).iter().map(|b| format!("{b:02x}")).collect();
    if digest != ICLOUD_SETUP_SHA256 {
        return Err("the download isn't the iCloudSetup.exe expected".into());
    }
    Ok(setup)
}

/// The first source that delivers the expected file.
async fn fetch_icloud_setup() -> Result<Vec<u8>, String> {
    let mut why = Vec::new();
    for (n, url) in ICLOUD_SETUP_SOURCES.iter().enumerate() {
        let host = url.split('/').nth(2).unwrap_or(url);
        if n > 0 {
            say!("trying another source for iCloud for Windows 7.21 ({host})");
        }
        match fetch_from(url).await {
            Ok(setup) => return Ok(setup),
            Err(e) => {
                crate::log_line(&format!("iCloudSetup.exe from {host}: {e}"));
                why.push(format!("{host}: {e}"));
            }
        }
    }
    Err(format!("iCloud for Windows 7.21 could not be downloaded ({})", why.join("; ")))
}

async fn unpack_icloud(home: &Path) -> Result<(), String> {"""
    if text.count(old) != 1:
        sys.exit('anisette.rs: unpack_icloud moved; update patch-appleid.py')
    text = text.replace(old, new, 1)
    path.write_text(text, encoding='utf-8')
    print('appleid: iCloud download from three sources, streamed with progress, SHA-256 checked')

# -27952 / HTTP 503 fallback: some accounts refuse iCloud/7.21 with GSA error
# -27952 ("Update iCloud for Windows to the latest version to sign in"), and
# Apple's GSA backend/edge can reject synthetic iCloud/15.10 headers with
# immediate HTTP 503 (#42). Keep 7.21 by default so accounts that never see
# -27952 send the exact same headers as before; on -27952 (and on HTTP 503/403
# once off 7.21), step through verified real client profiles (macos-akd,
# itunes, icloud-15) and persist the working profile name in APPLEID_OUT\client.txt
# for --refresh and --contacts.
# (aidc.rs uses its own sharingd/2.0 CLIENT_INFO, not gsa::CLIENT_INFO, so it is
# left untouched.)
CLIENT_MARK = 'nekodrop: -27952 client info'

path = pathlib.Path(sys.argv[1]) / 'crates' / 'appleid' / 'src' / 'gsa.rs'
text = path.read_text(encoding='utf-8')
if CLIENT_MARK not in text:
    old = 'pub const CLIENT_INFO: &str = "<PC> <Windows;10.0.26100;> <com.apple.AuthKit/1 (com.apple.iCloud/7.21)>";'
    new = '''pub const CLIENT_INFO: &str = "<PC> <Windows;10.0.26100;> <com.apple.AuthKit/1 (com.apple.iCloud/7.21)>";
/// nekodrop: -27952 client info. Ordered X-MMe-Client-Info profiles tried on
/// -27952 (or HTTP 503/403 on a fallback profile, #42).
///   0: iCloud 7.21 (WirDrop default, unchanged for accounts that accept it)
///   1: macOS akd (matches AGENT; bypasses the Windows iCloud -27952 gate)
///   2: Windows iTunes 12.12 (Windows CoreADI GSA profile)
///   3: Windows AuthKitWin + iCloud 15.3 (real Windows iCloud 15 header)
pub const CLIENT_PROFILES: [(&str, &str); 4] = [
    ("7.21", CLIENT_INFO),
    ("icloud-15", "<PC> <Windows;10.0.26100;> <com.apple.AuthKitWin/1 (com.apple.iCloud/15.3.149)>"),
    ("itunes", "<PC> <Windows;6.2(0,0);9200> <com.apple.AuthKit/1 (com.apple.iTunes/12.12.4.1)>"),
    ("macos-akd", "<MacPro7,1> <Mac OS X;15.3.1;24D70> <com.apple.AuthKit/1 (com.apple.akd/1.0)>"),
];
static CLIENT_PROFILE_IDX: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);

pub fn client_profile_idx() -> usize {
    CLIENT_PROFILE_IDX
        .load(std::sync::atomic::Ordering::Relaxed)
        .min(CLIENT_PROFILES.len() - 1)
}

pub fn set_client_profile_idx(idx: usize) {
    CLIENT_PROFILE_IDX.store(
        idx.min(CLIENT_PROFILES.len() - 1),
        std::sync::atomic::Ordering::Relaxed,
    );
}

pub fn set_client_profile_by_name(name: &str) -> bool {
    // Migrate legacy "15.10" saved by earlier nekodrop versions to icloud-15.
    let target = if name == "15.10" { "icloud-15" } else { name };
    if let Some(idx) = CLIENT_PROFILES.iter().position(|(n, _)| *n == target) {
        set_client_profile_idx(idx);
        true
    } else {
        false
    }
}

pub fn client_profile_name() -> &'static str {
    CLIENT_PROFILES[client_profile_idx()].0
}

pub fn client_info() -> &'static str {
    CLIENT_PROFILES[client_profile_idx()].1
}'''
    if text.count(old) != 1:
        sys.exit('gsa.rs: CLIENT_INFO moved; update patch-appleid.py')
    text = text.replace(old, new, 1)

    old_call = '''async fn call(request: Dictionary) -> Result<Dictionary, Fault> {
    let mut header = Dictionary::new();
    header.insert("Version".into(), "1.0.1".into());
    let mut envelope = Dictionary::new();
    envelope.insert("Header".into(), Value::Dictionary(header));
    envelope.insert("Request".into(), Value::Dictionary(request));
    let mut body = Vec::new();
    plist::to_writer_xml(&mut body, &Value::Dictionary(envelope)).map_err(|e| Fault::Other(e.to_string()))?;
    let res = crate::http::client()
        .post(SERVICE)
        .header("Content-Type", "text/x-xml-plist")
        .header("Accept", "*/*")
        .header("User-Agent", AGENT)
        .header("X-MMe-Client-Info", CLIENT_INFO)
        .body(body)
        .send()
        .await
        .map_err(|e| Fault::Unreachable(e.to_string()))?;
    let status = res.status();
    let bytes = res.bytes().await.map_err(|e| Fault::Unreachable(e.to_string()))?;
    let Ok(Value::Dictionary(mut reply)) = plist::from_bytes::<Value>(&bytes) else {
        // plist 대신 오류 페이지: GrandSlam이 바쁨(503) 같은 것.
        return Err(Fault::Busy(status.as_u16()));
    };'''
    new_call = '''async fn call(request: Dictionary) -> Result<Dictionary, Fault> {
    let op = request.get("o").and_then(Value::as_string).unwrap_or("?").to_string();
    let mut header = Dictionary::new();
    header.insert("Version".into(), "1.0.1".into());
    let mut envelope = Dictionary::new();
    envelope.insert("Header".into(), Value::Dictionary(header));
    envelope.insert("Request".into(), Value::Dictionary(request));
    let mut body = Vec::new();
    plist::to_writer_xml(&mut body, &Value::Dictionary(envelope)).map_err(|e| Fault::Other(e.to_string()))?;
    let res = crate::http::client()
        .post(SERVICE)
        .header("Content-Type", "text/x-xml-plist")
        .header("Accept", "*/*")
        .header("User-Agent", AGENT)
        .header("X-MMe-Client-Info", client_info())
        .body(body)
        .send()
        .await
        .map_err(|e| Fault::Unreachable(e.to_string()))?;
    let status = res.status();
    let ct = res
        .headers()
        .get(reqwest::header::CONTENT_TYPE)
        .and_then(|v| v.to_str().ok())
        .unwrap_or("-")
        .to_string();
    let bytes = res.bytes().await.map_err(|e| Fault::Unreachable(e.to_string()))?;
    let Ok(Value::Dictionary(mut reply)) = plist::from_bytes::<Value>(&bytes) else {
        // Non-plist error page (e.g. HTTP 503/403 from GSA edge): log safe diagnostic metadata (#42).
        let snippet: String = String::from_utf8_lossy(&bytes)
            .chars()
            .map(|c| if c.is_ascii_control() { ' ' } else { c })
            .take(120)
            .collect();
        crate::log_line(&format!(
            "gsa {op} ({}): HTTP {status}, content-type={ct}, body={}",
            client_profile_name(),
            snippet.trim()
        ));
        return Err(Fault::Busy(status.as_u16()));
    };'''
    if text.count(old_call) != 1:
        sys.exit('gsa.rs: call() moved; update patch-appleid.py')
    text = text.replace(old_call, new_call, 1)

    old = '        ("X-MMe-Client-Info".into(), CLIENT_INFO.into()),'
    new = '        ("X-MMe-Client-Info".into(), client_info().into()),'
    if text.count(old) != 1:
        sys.exit('gsa.rs: code_headers CLIENT_INFO moved; update patch-appleid.py')
    text = text.replace(old, new, 1)

    old = '''async fn validate_code(session: &Session, anisette: &mut Anisette, code: &str) -> Result<(), String> {
    let headers = code_headers(session, anisette)?;
    let res = request(reqwest::Method::GET, &format!("{SERVICE}/validate"), &headers)
        .header("Content-Type", "text/x-xml-plist")
        .header("Accept", "text/x-xml-plist")
        .header("security-code", code.trim())
        .send()
        .await
        .map_err(|e| e.to_string())?;
    let status = res.status();
    let bytes = res.bytes().await.map_err(|e| e.to_string())?;
    match plist::from_bytes::<Value>(&bytes) {
        Ok(Value::Dictionary(reply)) => check(&reply).map_err(|e| format!("the verification code was refused: {e}")),
        _ => Err(format!("the verification code was refused (HTTP {status})")),
    }
}'''
    new = '''async fn validate_code(session: &Session, anisette: &mut Anisette, code: &str) -> Result<(), String> {
    // nekodrop: retry transient 502/503/504 from /validate and fall back to
    // /auth/verify/trusteddevice/securitycode when GrandSlam /validate returns 503 (#42).
    let trimmed = code.trim();
    for (attempt, wait) in [5u64, 10, 15, 0].into_iter().enumerate() {
        let headers = code_headers(session, anisette)?;
        let res = request(reqwest::Method::GET, &format!("{SERVICE}/validate"), &headers)
            .header("Content-Type", "text/x-xml-plist")
            .header("Accept", "text/x-xml-plist")
            .header("security-code", trimmed)
            .send()
            .await
            .map_err(|e| e.to_string())?;
        let status = res.status();
        if matches!(status.as_u16(), 502 | 503 | 504) {
            if let Ok(fb_headers) = code_headers(session, anisette) {
                let body = serde_json::json!({ "securityCode": { "code": trimmed } });
                if let Ok(fb) = request(reqwest::Method::POST, &format!("{AUTH}/verify/trusteddevice/securitycode"), &fb_headers)
                    .header("Accept", "application/json")
                    .json(&body)
                    .send()
                    .await
                    && fb.status().is_success()
                {
                    say!("verification code accepted via fallback endpoint");
                    return Ok(());
                }
            }
            if wait == 0 {
                return Err(format!("Apple's verification server is still unavailable (HTTP {status})"));
            }
            say!("Apple's verification server is busy (HTTP {status}); retrying in {wait} s ({}/3)", attempt + 1);
            tokio::time::sleep(std::time::Duration::from_secs(wait)).await;
            continue;
        }
        let bytes = res.bytes().await.map_err(|e| e.to_string())?;
        return match plist::from_bytes::<Value>(&bytes) {
            Ok(Value::Dictionary(reply)) => check(&reply).map_err(|e| format!("the verification code was refused: {e}")),
            _ => Err(format!("the verification code was refused (HTTP {status})")),
        };
    }
    unreachable!()
}'''
    if text.count(old) != 1:
        sys.exit('gsa.rs: validate_code moved; update patch-appleid.py')
    text = text.replace(old, new, 1)

    old_first = '    let (session, next) = password_round(account, password, anisette).await?;'
    new_first = '''    // nekodrop: retry transient 503/unreachable on the initial password_round (#42).
    // On a fallback profile (idx > 0), fail fast after 1 retry so main.rs can step
    // to the next client profile instead of burning 45 s on a rejected header.
    let (session, next) = {
        let max_tries: u32 = if client_profile_idx() == 0 { 2 } else { 1 };
        let mut round_tries = 0u32;
        loop {
            match password_round(account, password, anisette).await {
                Ok(pair) => break pair,
                Err(e @ (Fault::Busy(502 | 503 | 504) | Fault::Unreachable(_))) if round_tries < max_tries => {
                    round_tries += 1;
                    let wait = if client_profile_idx() == 0 { 5 + (round_tries as u64 - 1) * 5 } else { 5 };
                    say!("Apple's sign-in server is busy ({}); retrying in {wait} s ({round_tries}/{max_tries})", e.brief());
                    tokio::time::sleep(std::time::Duration::from_secs(wait)).await;
                }
                Err(e) => return Err(e.into()),
            }
        }
    };'''
    if text.count(old_first) != 1:
        sys.exit('gsa.rs: first password_round moved; update patch-appleid.py')
    text = text.replace(old_first, new_first, 1)

    path.write_text(text, encoding='utf-8')
    print('appleid: gsa.rs multi-profile client_info(), 503 diagnostics/retries and 2FA fallback')

path = pathlib.Path(sys.argv[1]) / 'crates' / 'appleid' / 'src' / 'anisette.rs'
text = path.read_text(encoding='utf-8')
if CLIENT_MARK not in text:
    old = '            ("X-MMe-Client-Info", crate::gsa::CLIENT_INFO.to_string()),'
    new = '            ("X-MMe-Client-Info", crate::gsa::client_info().to_string()), // nekodrop: -27952 client info'
    if text.count(old) != 1:
        sys.exit('anisette.rs: CLIENT_INFO header moved; update patch-appleid.py')
    text = text.replace(old, new, 1)
    path.write_text(text, encoding='utf-8')
    print('appleid: anisette.rs uses gsa::client_info()')

path = pathlib.Path(sys.argv[1]) / 'crates' / 'appleid' / 'src' / 'contacts.rs'
text = path.read_text(encoding='utf-8')
if CLIENT_MARK not in text:
    old = '            .header("X-MMe-Client-Info", crate::gsa::CLIENT_INFO)'
    new = '            .header("X-MMe-Client-Info", crate::gsa::client_info()) // nekodrop: -27952 client info'
    if text.count(old) != 1:
        sys.exit('contacts.rs: delegates CLIENT_INFO header moved; update patch-appleid.py')
    text = text.replace(old, new, 1)

    old = '        .header("X-MMe-Client-Info", crate::gsa::CLIENT_INFO)'
    new = '        .header("X-MMe-Client-Info", crate::gsa::client_info())'
    if text.count(old) != 1:
        sys.exit('contacts.rs: dav CLIENT_INFO header moved; update patch-appleid.py')
    text = text.replace(old, new, 1)

    path.write_text(text, encoding='utf-8')
    print('appleid: contacts.rs uses gsa::client_info()')

path = pathlib.Path(sys.argv[1]) / 'crates' / 'appleid' / 'src' / 'main.rs'
text = path.read_text(encoding='utf-8')
if CLIENT_MARK not in text:
    old = 'const APP_TOKEN: &str = "gs-token.txt";'
    new = '''const APP_TOKEN: &str = "gs-token.txt";
/// nekodrop: -27952 client info. Persists the working client profile for --refresh/--contacts.
const CLIENT_FILE: &str = "client.txt";

fn load_client_info(out: &Path) {
    if let Ok(text) = std::fs::read_to_string(out.join(CLIENT_FILE)) {
        gsa::set_client_profile_by_name(text.trim());
    }
}

fn save_client_info(out: &Path, profile: &str) {
    let _ = std::fs::write(out.join(CLIENT_FILE), format!("{profile}\\n"));
}'''
    if text.count(old) != 1:
        sys.exit('main.rs: APP_TOKEN moved; update patch-appleid.py')
    text = text.replace(old, new, 1)

    old = '''    let _ = std::fs::create_dir_all(&out);
    let code = match std::env::args().nth(1).as_deref() {'''
    new = '''    let _ = std::fs::create_dir_all(&out);
    load_client_info(&out);
    let code = match std::env::args().nth(1).as_deref() {'''
    if text.count(old) != 1:
        sys.exit('main.rs: main() setup moved; update patch-appleid.py')
    text = text.replace(old, new, 1)

    old = '    let session = gsa::sign_in(&account, &password, &mut anisette, &mut ask_code).await?;'
    new = '''    let session = {
        let mut idx = gsa::client_profile_idx();
        loop {
            match gsa::sign_in(&account, &password, &mut anisette, &mut ask_code).await {
                Ok(session) => {
                    if idx > 0 {
                        save_client_info(out, gsa::client_profile_name());
                    } else {
                        let _ = std::fs::remove_file(out.join(CLIENT_FILE));
                    }
                    break session;
                }
                Err(e) => {
                    let retryable = e.contains("-27952")
                        || e.contains("-23782")
                        || (idx > 0
                            && ["-80045", "-80009", "-22406", "-22410", "HTTP 503", "HTTP 403"]
                                .iter()
                                .any(|k| e.contains(k)));
                    if retryable && idx + 1 < gsa::CLIENT_PROFILES.len() {
                        let prev = gsa::client_profile_name();
                        idx += 1;
                        gsa::set_client_profile_idx(idx);
                        let next = gsa::client_profile_name();
                        say!("Apple refused {prev} ({e}); retrying with {next}");
                        tokio::time::sleep(std::time::Duration::from_secs(2)).await;
                        continue;
                    }
                    return Err(e);
                }
            }
        }
    };'''
    if text.count(old) != 1:
        sys.exit('main.rs: gsa::sign_in call moved; update patch-appleid.py')
    text = text.replace(old, new, 1)

    path.write_text(text, encoding='utf-8')
    print('appleid: main.rs steps through fallback GSA profiles on -27952/503/403 and persists to client.txt')
