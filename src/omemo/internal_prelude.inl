
using namespace weechat::xmpp;

constexpr std::string_view kOmemoNs = "urn:xmpp:omemo:2";
constexpr std::string_view kDevicesNode = "urn:xmpp:omemo:2:devices";
constexpr std::string_view kBundlesNode = "urn:xmpp:omemo:2:bundles";

// Legacy OMEMO (eu.siacs.conversations.axolotl / "OMEMO:1") constants
constexpr std::string_view kLegacyOmemoNs = "eu.siacs.conversations.axolotl";
constexpr std::string_view kLegacyDevicesNode = "eu.siacs.conversations.axolotl.devicelist";
// Legacy bundles use per-device nodes: this prefix + deviceId
constexpr std::string_view kLegacyBundlesNodePrefix = "eu.siacs.conversations.axolotl.bundles:";

constexpr std::string_view kDeviceIdKey = "device_id";
constexpr std::string_view kRegistrationIdKey = "registration_id";
constexpr std::string_view kIdentityPublicKey = "identity:public";
constexpr std::string_view kIdentityPrivateKey = "identity:private";
constexpr std::string_view kSignedPreKeyId = "signed_pre_key:id";
constexpr std::string_view kSignedPreKeyRecord = "signed_pre_key:record";
constexpr std::string_view kSignedPreKeyPublic = "signed_pre_key:public";
constexpr std::string_view kSignedPreKeySignature = "signed_pre_key:signature";
// Unix timestamp (seconds) of when the current signed prekey was generated.
// Absent means the SPK was created before rotation tracking was added; treat
// as expired immediately so a fresh one is generated on the next bundle call.
constexpr std::string_view kSignedPreKeyTimestamp = "signed_pre_key:generated_at";
// Rotate the signed prekey after 7 days (Signal spec recommendation: 1–4 weeks).
constexpr std::int64_t kSignedPreKeyRotationSecs = 7 * 24 * 60 * 60;
constexpr std::string_view kPrekeys = "prekeys";
constexpr std::string_view kOmemoPayloadInfo = "OMEMO Payload";
constexpr std::uint32_t kPreKeyStart = 1;
constexpr std::uint32_t kPreKeyCount = 100;
constexpr std::uint32_t kMinPreKeyCount = 25;
constexpr std::uint32_t kMaxOmemoDeviceId = 0x7fffffffU;

struct bundle_metadata {
    std::string signed_pre_key_id;
    std::string signed_pre_key;
    std::string signed_pre_key_signature;
    std::string identity_key;
    std::vector<std::pair<std::string, std::string>> prekeys;
};

struct signal_store_state {
    signal_protocol_identity_key_store identity {};
    signal_protocol_pre_key_store pre_key {};
    signal_protocol_signed_pre_key_store signed_pre_key {};
    signal_protocol_session_store session {};
    signal_protocol_sender_key_store sender_key {};
    signal_crypto_provider crypto {};
};

struct signal_address_view {
    std::string name;
    signal_protocol_address address {};
};

std::unordered_map<omemo *, std::unique_ptr<signal_store_state>> g_signal_store_states;

[[nodiscard]] auto make_signal_address(std::string_view jid, std::int32_t device_id)
    -> signal_address_view;

[[nodiscard]] auto pkcs7_unpad(const std::vector<std::uint8_t> &padded) -> std::optional<std::string>;

struct signal_buffer_deleter {
    void operator()(signal_buffer *buffer) const noexcept
    {
        if (buffer)
            signal_buffer_free(buffer);
    }
};

using unique_signal_buffer = std::unique_ptr<signal_buffer, signal_buffer_deleter>;

struct pre_key_list_deleter {
    void operator()(signal_protocol_key_helper_pre_key_list_node *node) const noexcept
    {
        if (node)
            signal_protocol_key_helper_key_list_free(node);
    }
};

using unique_pre_key_list =
    std::unique_ptr<signal_protocol_key_helper_pre_key_list_node, pre_key_list_deleter>;

struct gcry_cipher_deleter {
    void operator()(gcry_cipher_hd_t handle) const noexcept
    {
        if (handle)
            gcry_cipher_close(handle);
    }
};

using unique_gcry_cipher = std::unique_ptr<std::remove_pointer_t<gcry_cipher_hd_t>, gcry_cipher_deleter>;

struct gcry_mac_deleter {
    void operator()(gcry_mac_hd_t handle) const noexcept
    {
        if (handle)
            gcry_mac_close(handle);
    }
};

using unique_gcry_mac = std::unique_ptr<std::remove_pointer_t<gcry_mac_hd_t>, gcry_mac_deleter>;

struct omemo2_payload {
    // 32-byte random message key
    std::array<std::uint8_t, 32> key {};
    // 16-byte truncated HMAC-SHA-256 of ciphertext (transport-key bundle = key || hmac)
    std::array<std::uint8_t, 16> hmac {};
    // AES-256-CBC ciphertext only (IV is derived from key via HKDF, not transmitted)
    std::vector<std::uint8_t> payload;
};

struct omemo2_keys {
    std::array<std::uint8_t, 32> encryption {};
    std::array<std::uint8_t, 32> authentication {};
    std::array<std::uint8_t, 16> iv {};
};

using c_string = std::unique_ptr<char, decltype(&free)>;

[[nodiscard]] auto eval_path(const std::string &expression) -> std::string
{
    c_string value {
        weechat_string_eval_expression(expression.c_str(), nullptr, nullptr, nullptr),
        &free,
    };
    return value ? std::string {value.get()} : std::string {};
}

[[nodiscard]] auto make_db_path(std::string_view account_name) -> std::string
{
    return eval_path(fmt::format("${{weechat_data_dir}}/xmpp/omemo_{}.db", account_name));
}

[[nodiscard]] auto normalize_bare_jid([[maybe_unused]] xmpp_ctx_t *context, std::string_view jid) -> std::string
{
    const std::string bare = ::jid(nullptr, std::string {jid}).bare;
    return bare.empty() ? std::string {jid} : bare;
}

void request_devicelist(weechat::account &account, std::string_view jid)
{
    const std::string target_jid = normalize_bare_jid(account.context, jid);
    if (account.omemo.missing_omemo2_devicelist.count(target_jid) != 0)
        return;
    account.omemo.missing_omemo2_devicelist.insert(target_jid);

    const std::string uuid = stanza::uuid(account.context);
    if (!uuid.empty())
        account.omemo.pending_iq_jid[uuid] = target_jid;

    auto items_el = stanza::xep0060::items(kDevicesNode);
    auto pubsub_el = stanza::xep0060::pubsub();
    pubsub_el.items(items_el);
    auto iq_s = stanza::iq()
        .type("get")
        .id(uuid)
        .from(account.jid())
        .to(target_jid);
    static_cast<stanza::xep0060::iq&>(iq_s).pubsub(pubsub_el);
    auto stanza_ptr = iq_s.build(account.context);
    account.connection.send(stanza_ptr.get());
}

void request_bundle(weechat::account &account, std::string_view jid, std::uint32_t device_id)
{
    const std::string target_jid = normalize_bare_jid(account.context, jid);
    const std::string own_bare_jid = normalize_bare_jid(account.context, account.jid());
    const bool is_own_jid = weechat_strcasecmp(target_jid.c_str(), own_bare_jid.c_str()) == 0;
    if (!is_own_jid && !account.omemo.has_peer_traffic(account.context, target_jid))
    {
        XDEBUG("omemo: deferring OMEMO:2 bundle request for {}/{} until PM/MAM traffic is observed",
               target_jid, device_id);
        return;
    }

    const auto key = std::make_pair(target_jid, device_id);
    if (account.omemo.pending_bundle_fetch.count(key))
    {
        XDEBUG("omemo: bundle request for {}/{} already pending (skipping duplicate)",
               target_jid, device_id);
        return;
    }

    const auto item_id = fmt::format("{}", device_id);
    const std::string uuid = stanza::uuid(account.context);
    if (!uuid.empty())
        account.omemo.pending_iq_jid[uuid] = target_jid;
    account.omemo.pending_bundle_fetch.insert(key);

    XDEBUG("omemo: sent bundle request for {}/{} (uuid={})", target_jid, device_id, uuid.empty() ? "?" : uuid);

    auto items_el = stanza::xep0060::items(kBundlesNode);
    items_el.item(stanza::xep0060::item().id(item_id));
    auto pubsub_el = stanza::xep0060::pubsub();
    pubsub_el.items(items_el);
    auto iq_s = stanza::iq()
        .type("get")
        .id(uuid)
        .from(account.jid())
        .to(target_jid);
    static_cast<stanza::xep0060::iq&>(iq_s).pubsub(pubsub_el);
    auto stanza_ptr = iq_s.build(account.context);
    account.connection.send(stanza_ptr.get());
}

void print_info(t_gui_buffer *buffer, std::string_view message)
{
    weechat_printf(buffer, "%s%s", weechat_prefix("network"), std::string {message}.c_str());
}

void print_error(t_gui_buffer *buffer, std::string_view message)
{
    weechat_printf(buffer, "%s%s", weechat_prefix("error"), std::string {message}.c_str());
}

[[nodiscard]] auto split(std::string_view input, char separator) -> std::vector<std::string>
{
    std::vector<std::string> parts;
    std::string current;

    for (const char ch : input)
    {
        if (ch == separator)
        {
            if (!current.empty())
            {
                parts.push_back(current);
                current.clear();
            }
            continue;
        }

        current.push_back(ch);
    }

    if (!current.empty())
        parts.push_back(current);

    return parts;
}

[[nodiscard]] auto join(const std::vector<std::string> &values, std::string_view separator) -> std::string
{
    std::ostringstream stream;

    for (std::size_t index = 0; index < values.size(); ++index)
    {
        if (index != 0)
            stream << separator;
        stream << values[index];
    }

    return stream.str();
}

[[nodiscard]] auto random_device_id() -> std::uint32_t
{
    std::random_device random_device;
    std::mt19937 generator {random_device()};
    std::uniform_int_distribution<std::uint32_t> distribution {1, kMaxOmemoDeviceId};
    return distribution(generator);
}

[[nodiscard]] constexpr auto is_valid_omemo_device_id(std::uint32_t device_id) -> bool
{
    return device_id != 0 && device_id <= kMaxOmemoDeviceId;
}

[[nodiscard]] auto parse_uint32(std::string_view value) -> std::optional<std::uint32_t>
{
    std::uint32_t parsed = 0;
    const auto *begin = value.data();
    const auto *end = value.data() + value.size();
    const auto [ptr, error] = std::from_chars(begin, end, parsed);
    if (error != std::errc {} || ptr != end)
        return std::nullopt;
    return parsed;
}

[[nodiscard]] auto parse_int64(std::string_view value) -> std::optional<std::int64_t>
{
    std::int64_t parsed = 0;
    const auto *begin = value.data();
    const auto *end = value.data() + value.size();
    const auto [ptr, error] = std::from_chars(begin, end, parsed);
    if (error != std::errc {} || ptr != end)
        return std::nullopt;
    return parsed;
}

[[nodiscard]] auto key_for_devicelist(std::string_view jid) -> std::string
{
    return fmt::format("devicelist:{}", jid);
}

[[nodiscard]] auto key_for_bundle(std::string_view jid, std::uint32_t device_id) -> std::string
{
    return fmt::format("bundle:{}:{}", jid, device_id);
}

// Axolotl OMEMO uses separate LMDB keys so we can distinguish which protocol
// version a peer advertises (they may have both, though unlikely).
[[nodiscard]] auto key_for_axolotl_devicelist(std::string_view jid) -> std::string
{
    return fmt::format("axolotl_devicelist:{}", jid);
}

// Axolotl bundles share the same Signal session store as OMEMO:2 bundles — the
// Signal crypto is identical.  Only the XMPP stanza wrapping differs.
// We use a separate LMDB key prefix so we can tell which namespace the bundle
// came from when deciding which encode path to use.
[[nodiscard]] auto key_for_axolotl_bundle(std::string_view jid, std::uint32_t device_id) -> std::string
{
    return fmt::format("axolotl_bundle:{}:{}", jid, device_id);
}

[[nodiscard]] auto key_for_device_mode(std::string_view jid, std::uint32_t device_id) -> std::string
{
    return fmt::format("device_mode:{}:{}", jid, device_id);
}

void store_device_mode(omemo &self,
                       std::string_view jid,
                       std::uint32_t device_id,
                       omemo::peer_mode mode)
{
    if (jid.empty() || device_id == 0)
        return;

    const char *mode_value = nullptr;
    if (mode == omemo::peer_mode::omemo2)
        mode_value = "omemo2";
    else if (mode == omemo::peer_mode::axolotl)
        mode_value = "axolotl";
    else
        return;

    if (!self.db_env)
        return;

    auto transaction = lmdb::txn::begin(self.db_env);
    self.dbi.omemo.put(transaction, key_for_device_mode(jid, device_id), mode_value);
    transaction.commit();
}

[[nodiscard]] auto load_device_mode(omemo &self,
                                    std::string_view jid,
                                    std::uint32_t device_id)
    -> std::optional<omemo::peer_mode>
{
    if (jid.empty() || device_id == 0)
        return std::nullopt;

    if (!self.db_env)
        return std::nullopt;

    auto transaction = lmdb::txn::begin(self.db_env, nullptr, MDB_RDONLY);
    std::string_view mode_value;
    if (!self.dbi.omemo.get(transaction, key_for_device_mode(jid, device_id), mode_value))
        return std::nullopt;

    if (mode_value == "omemo2")
        return omemo::peer_mode::omemo2;
    // Accept both "axolotl" (new) and "legacy" (old DB) so existing databases
    // do not lose device-mode knowledge after the rename migration.
    if (mode_value == "axolotl" || mode_value == "legacy")
        return omemo::peer_mode::axolotl;

    return std::nullopt;
}

[[nodiscard]] auto key_for_session(std::string_view jid, std::uint32_t device_id) -> std::string
{
    return fmt::format("session:{}:{}", jid, device_id);
}

[[nodiscard]] auto key_for_identity(std::string_view jid, std::int32_t device_id) -> std::string
{
    return fmt::format("identity:{}:{}", jid, device_id);
}

[[nodiscard]] auto key_for_prekey_record(std::uint32_t id) -> std::string
{
    return fmt::format("prekey:{}", id);
}

// XEP-0450: ATM trust level storage.
// key_b64 is the Base64-encoded SHA-256 fingerprint of the raw identity key.
[[nodiscard]] auto key_for_atm_trust(std::string_view jid, std::string_view key_b64) -> std::string
{
    return fmt::format("atm_trust:{}:{}", jid, key_b64);
}

// Values: "trusted", "distrusted", "undecided"
void store_atm_trust(omemo &self,
                     std::string_view jid,
                     std::string_view key_b64,
                     std::string_view level)
{
    if (!self.db_env || jid.empty() || key_b64.empty())
        return;
    auto txn = lmdb::txn::begin(self.db_env);
    self.dbi.omemo.put(txn, key_for_atm_trust(jid, key_b64), std::string(level));
    txn.commit();
}

[[nodiscard]] auto load_atm_trust(omemo &self,
                                   std::string_view jid,
                                   std::string_view key_b64)
    -> std::optional<std::string>
{
    if (!self.db_env || jid.empty() || key_b64.empty())
        return std::nullopt;
    auto txn = lmdb::txn::begin(self.db_env, nullptr, MDB_RDONLY);
    std::string_view value;
    if (!self.dbi.omemo.get(txn, key_for_atm_trust(jid, key_b64), value))
        return std::nullopt;
    return std::string(value);
}

[[nodiscard]] auto key_for_signed_prekey_record(std::uint32_t id) -> std::string
{
    return fmt::format("signed-prekey:{}", id);
}

[[nodiscard]] auto key_for_sender_key(std::string_view group,
                                      std::string_view jid,
                                      std::int32_t device_id) -> std::string
{
    return fmt::format("senderkey:{}:{}:{}", group, jid, device_id);
}

[[nodiscard]] auto stanza_text(xmpp_stanza_t *stanza) -> std::string
{
    if (!stanza)
        return {};

    xmpp_ctx_t *ctx = xmpp_stanza_get_context(stanza);
    struct ctx_free {
        xmpp_ctx_t *ctx;
        void operator()(char *p) const noexcept { if (p && ctx) xmpp_free(ctx, p); }
    };
    std::unique_ptr<char, ctx_free> text_ptr { xmpp_stanza_get_text(stanza), ctx_free{ctx} };
    return (text_ptr && *text_ptr) ? std::string {text_ptr.get()} : std::string {};
}

[[nodiscard]] auto base64_encode_raw(const std::uint8_t *data, std::size_t size) -> std::string
{
    if (!data || size == 0)
        return {};

    const int encoded_size = 4 * static_cast<int>((size + 2) / 3) + 1;
    std::string encoded(static_cast<std::size_t>(encoded_size), '\0');
    const int written = weechat_string_base_encode("64",
        reinterpret_cast<const char *>(data), static_cast<int>(size), encoded.data());
    if (written <= 0)
        return {};
    encoded.resize(static_cast<std::size_t>(written));
    return encoded;
}

[[nodiscard]] auto base64_encode([[maybe_unused]] xmpp_ctx_t *context,
                                 const unsigned char *data, std::size_t size) -> std::string
{
    return base64_encode_raw(data, size);
}

// Compute Base64-encoded SHA-256 fingerprint of raw bytes — used as ATM key identifier.
[[nodiscard]] auto atm_fingerprint_b64(const std::vector<std::uint8_t> &key_bytes) -> std::string
{
    if (key_bytes.empty())
        return {};
    unsigned char digest[32];
    gcry_md_hash_buffer(GCRY_MD_SHA256, digest, key_bytes.data(), key_bytes.size());
    return base64_encode_raw(digest, sizeof(digest));
}

[[nodiscard]] auto base64_decode([[maybe_unused]] xmpp_ctx_t *context, std::string_view encoded)
    -> std::vector<std::uint8_t>
{
    if (encoded.empty())
        return {};

    // Upper bound: base64 expands 3 bytes → 4 chars, so decoded ≤ 3*(len/4)+3
    const std::size_t max_decoded = (encoded.size() / 4 + 1) * 3 + 3;
    std::vector<char> buf(max_decoded, '\0');
    // encoded must be NUL-terminated; use a temporary std::string
    const std::string encoded_str {encoded};
    const int written = weechat_string_base_decode("64", encoded_str.c_str(), buf.data());
    if (written <= 0)
        return {};

    const auto *ubuf = reinterpret_cast<const std::uint8_t *>(buf.data());
    return std::vector<std::uint8_t>(ubuf, ubuf + static_cast<std::size_t>(written));
}

[[nodiscard]] auto utc_timestamp_now() -> std::string
{
    const std::time_t now = std::time(nullptr);
    std::tm utc {};
    gmtime_r(&now, &utc);

    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

[[nodiscard]] auto xml_escape(std::string_view text) -> std::string
{
    std::string escaped;
    escaped.reserve(text.size());
    for (const char ch : text)
    {
        switch (ch)
        {
            case '&': escaped += "&amp;"; break;
            case '<': escaped += "&lt;"; break;
            case '>': escaped += "&gt;"; break;
            case '\"': escaped += "&quot;"; break;
            case '\'': escaped += "&apos;"; break;
            default: escaped.push_back(ch); break;
        }
    }
    return escaped;
}

// Request the legacy OMEMO device list (eu.siacs.conversations.axolotl.devicelist) for jid.
void request_axolotl_devicelist(weechat::account &account, std::string_view jid)
{
    const std::string target_jid = normalize_bare_jid(account.context, jid);
    if (account.omemo.missing_axolotl_devicelist.count(target_jid) != 0)
        return;
    account.omemo.missing_axolotl_devicelist.insert(target_jid);

    const std::string uuid = stanza::uuid(account.context);
    if (!uuid.empty())
        account.omemo.pending_iq_jid[uuid] = target_jid;

    XDEBUG("omemo: requesting legacy device list for {}", target_jid);

    auto items_el = stanza::xep0060::items(kLegacyDevicesNode);
    auto pubsub_el = stanza::xep0060::pubsub();
    pubsub_el.items(items_el);
    auto iq_s = stanza::iq()
        .type("get")
        .id(uuid)
        .from(account.jid())
        .to(target_jid);
    static_cast<stanza::xep0060::iq&>(iq_s).pubsub(pubsub_el);
    auto stanza_ptr = iq_s.build(account.context);
    account.connection.send(stanza_ptr.get());
}

// Request a legacy OMEMO bundle (eu.siacs.conversations.axolotl.bundles:{device_id}) for jid.
// Legacy bundles use a per-device node (device ID embedded in node name, not item ID).
void request_axolotl_bundle(weechat::account &account, std::string_view jid, std::uint32_t device_id)
{
    const std::string target_jid = normalize_bare_jid(account.context, jid);
    const std::string own_bare_jid = normalize_bare_jid(account.context, account.jid());
    const bool is_own_jid = weechat_strcasecmp(target_jid.c_str(), own_bare_jid.c_str()) == 0;
    if (!is_own_jid && !account.omemo.has_peer_traffic(account.context, target_jid))
    {
        XDEBUG("omemo: deferring legacy bundle request for {}/{} until PM/MAM traffic is observed",
               target_jid, device_id);
        return;
    }

    const auto key = std::make_pair(target_jid, device_id);
    if (account.omemo.pending_bundle_fetch.count(key))
    {
        XDEBUG("omemo: legacy bundle request for {}/{} already pending (skipping)",
               target_jid, device_id);
        return;
    }

    // Per the Conversations protocol, bundle node = prefix + device_id (no separate item id)
    const auto bundle_node = fmt::format("{}{}", kLegacyBundlesNodePrefix, device_id);
    const std::string uuid = stanza::uuid(account.context);
    if (!uuid.empty())
        account.omemo.pending_iq_jid[uuid] = target_jid;
    account.omemo.pending_bundle_fetch.insert(key);

    XDEBUG("omemo: requesting legacy bundle for {}/{} (node={})", target_jid, device_id, bundle_node);

    auto items_el = stanza::xep0060::items(bundle_node);
    auto pubsub_el = stanza::xep0060::pubsub();
    pubsub_el.items(items_el);
    auto iq_s = stanza::iq()
        .type("get")
        .id(uuid)
        .from(account.jid())
        .to(target_jid);
    static_cast<stanza::xep0060::iq&>(iq_s).pubsub(pubsub_el);
    auto stanza_ptr = iq_s.build(account.context);
    account.connection.send(stanza_ptr.get());
}

// Generate a random padding string for SCE <rpad/> (XEP-0420 §4.3 MUST).
// Uses a random length in [1,64] and random alphanumeric characters.
[[nodiscard]] static auto make_rpad() -> std::string
{
    static const std::string_view kAlphanum =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    thread_local std::mt19937 rng { std::random_device{}() };
    std::uniform_int_distribution<std::size_t> len_dist { 1, 64 };
    std::uniform_int_distribution<std::size_t> char_dist { 0, kAlphanum.size() - 1 };
    const std::size_t len = len_dist(rng);
    std::string pad;
    pad.reserve(len);
    for (std::size_t i = 0; i < len; ++i)
        pad += kAlphanum[char_dist(rng)];
    return pad;
}

// Build an SCE envelope for the plaintext message body (XEP-0420 §4.3).
//  - <from jid='…'/>  always present (MUST per spec)
//  - <to jid='…'/>    MUST be included iff the message is addressed to a group (MUC/MIX)
//  - <time stamp='…'/>
//  - <rpad>…</rpad>  random-length random-content padding (MUST per spec)
[[nodiscard]] auto sce_wrap([[maybe_unused]] xmpp_ctx_t *context, weechat::account &account,
                            std::string_view plaintext,
                            std::string_view to_jid = {},
                            bool is_muc = false) -> std::string
{
    const char *bound = xmpp_conn_get_bound_jid(account.connection);
    const std::string from = bound ? ::jid(nullptr, bound).bare : std::string {};

    // XEP-0420 §4.3: <to/> MUST be included if and only if addressed to a group.
    const std::string to_elem = (is_muc && !to_jid.empty())
        ? fmt::format("<to jid='{}'/>" , xml_escape(std::string(to_jid)))
        : std::string {};

    return fmt::format(
        "<envelope xmlns='urn:xmpp:sce:1'><content><body xmlns='jabber:client'>{}</body></content>"
        "<time stamp='{}'/><from jid='{}'/>{}<rpad>{}</rpad></envelope>",
        xml_escape(plaintext),
        utc_timestamp_now(),
        xml_escape(from),
        to_elem,
        xml_escape(make_rpad()));
}

// Like sce_wrap() but places arbitrary pre-serialised XML as the sole child of
// <content> instead of <body>.  Used by the ATM trust message path where the
// content element is <trust-message xmlns='urn:xmpp:tm:1'> (XEP-0450 §4).
// content_xml must already be properly XML-serialised (not further escaped).
[[nodiscard]] auto sce_wrap_content([[maybe_unused]] xmpp_ctx_t *context,
                                    weechat::account &account,
                                    std::string_view content_xml,
                                    std::string_view to_jid = {},
                                    bool is_muc = false) -> std::string
{
    const char *bound = xmpp_conn_get_bound_jid(account.connection);
    const std::string from = bound ? ::jid(nullptr, bound).bare : std::string {};

    const std::string to_elem = (is_muc && !to_jid.empty())
        ? fmt::format("<to jid='{}'/>" , xml_escape(std::string(to_jid)))
        : std::string {};

    return fmt::format(
        "<envelope xmlns='urn:xmpp:sce:1'><content>{}</content>"
        "<time stamp='{}'/><from jid='{}'/>{}<rpad>{}</rpad></envelope>",
        content_xml,              // already serialised XML — not escaped again
        utc_timestamp_now(),
        xml_escape(from),
        to_elem,
        xml_escape(make_rpad()));
}

[[nodiscard]] auto pkcs7_pad(std::string_view plaintext, std::size_t block_size)
    -> std::vector<std::uint8_t>
{
    std::vector<std::uint8_t> output(plaintext.begin(), plaintext.end());
    const std::size_t actual_padding = (output.size() % block_size == 0) ? block_size : (block_size - (output.size() % block_size));
    output.insert(output.end(), actual_padding, static_cast<std::uint8_t>(actual_padding));
    return output;
}

[[nodiscard]] auto hmac_sha256(std::span<const std::uint8_t> key,
                               std::span<const std::uint8_t> data) -> std::optional<std::array<std::uint8_t, 32>>
{
    gcry_mac_hd_t mac_raw = nullptr;
    if (gcry_mac_open(&mac_raw, GCRY_MAC_HMAC_SHA256, 0, nullptr) != 0)
