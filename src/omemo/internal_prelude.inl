
using namespace weechat::xmpp;

// Axolotl (eu.siacs.conversations.axolotl) is the sole supported OMEMO namespace.
constexpr std::string_view kLegacyOmemoNs = "eu.siacs.conversations.axolotl";
constexpr std::string_view kLegacyDevicesNode = "eu.siacs.conversations.axolotl.devicelist";
// Bundles use per-device nodes: this prefix + deviceId
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
// Tracks the highest pre-key ID ever issued so that after a signed-prekey
// rotation we continue numbering rather than restarting at kPreKeyStart.
// Absent on first run — treated as kPreKeyStart - 1 (i.e., start at kPreKeyStart).
constexpr std::string_view kMaxPreKeyId = "max_pre_key_id";
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

[[nodiscard]] auto pkcs7_unpad(const std::vector<std::uint8_t> &padded) -> std::expected<std::string, std::string>;

struct signal_buffer_deleter {
    void operator()(signal_buffer *buffer) const noexcept
    {
        if (buffer)
            signal_buffer_free(buffer);
    }
};

using unique_signal_buffer = std::unique_ptr<signal_buffer, signal_buffer_deleter>;

// C ABI bridge: Signal protocol callbacks use void* for context pointers.
// make_signal_ctx<T>() creates a heap-allocated T and returns a raw T*
// (caller casts to void* when passing to Signal). free_signal_ctx<T>(void*)
// is called from Signal cleanup callbacks to destroy the object.
// These centralize the make_unique + release + delete sequence so the rest
// of the OMEMO code never touches raw allocation/deallocation.
template<typename T, typename... Args>
[[nodiscard]] auto make_signal_ctx(Args&&... args) -> T*
{
    return std::make_unique<T>(std::forward<Args>(args)...).release();
}
template<typename T>
void free_signal_ctx(void *ctx)
{
    delete static_cast<T*>(ctx);  // sole delete in OMEMO; required by Signal C API
}

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

// BTBV trust levels — mirrors omemo_dr/const.py OMEMOTrust
enum class omemo_trust : int {
    UNTRUSTED = 0, // explicitly distrusted by user
    VERIFIED  = 1, // manually verified (fingerprint/QR)
    UNDECIDED = 2, // new device for JID that already has VERIFIED or UNTRUSTED keys
    BLIND     = 3, // auto-trusted (TOFU — first contact with this JID)
};

using c_string = std::unique_ptr<char, decltype(&free)>;

[[nodiscard]] auto eval_path(std::string_view expression) -> std::string
{
    std::string expr_str(expression);  // ensure null-terminated for C API
    c_string value {
        weechat_string_eval_expression(expr_str.c_str(), nullptr, nullptr, nullptr),
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

    bool first = true;
    std::ranges::for_each(values, [&](const auto &v) {
        if (!first)
            stream << separator;
        first = false;
        stream << v;
    });

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

[[nodiscard]] auto key_for_tofu_trust(std::string_view jid, std::uint32_t device_id) -> std::string
{
    return fmt::format("trust:{}:{}", jid, device_id);
}

void store_tofu_trust(omemo &self,
                      std::string_view jid,
                      std::uint32_t device_id,
                      omemo_trust trust)
{
    if (!self.db_env || jid.empty() || device_id == 0)
        return;
    auto txn = lmdb::txn::begin(self.db_env);
    self.dbi.omemo.put(txn, key_for_tofu_trust(jid, device_id),
                       std::to_string(static_cast<int>(trust)));
    txn.commit();
}

[[nodiscard]] auto load_tofu_trust(omemo &self,
                                   std::string_view jid,
                                   std::uint32_t device_id)
    -> std::optional<omemo_trust>
{
    if (!self.db_env || jid.empty() || device_id == 0)
        return std::nullopt;
    auto txn = lmdb::txn::begin(self.db_env, nullptr, MDB_RDONLY);
    std::string_view value;
    if (!self.dbi.omemo.get(txn, key_for_tofu_trust(jid, device_id), value))
        return std::nullopt;
    const auto parsed = parse_uint32(value);
    if (!parsed || *parsed > 3)
        return std::nullopt;
    return static_cast<omemo_trust>(*parsed);
}

// BTBV get_default_trust() — mirrors Gajim storage/omemo.py:get_default_trust().
// Scans all trust:{jid}:* keys.  If any has value VERIFIED(1) or UNTRUSTED(0),
// new devices for that JID are UNDECIDED(2).  Otherwise BLIND(3).
[[nodiscard]] auto get_default_trust(omemo &self, std::string_view jid) -> omemo_trust
{
    if (!self.db_env || jid.empty())
        return omemo_trust::BLIND;

    const std::string prefix = fmt::format("trust:{}:", jid);
    auto txn = lmdb::txn::begin(self.db_env, nullptr, MDB_RDONLY);
    auto cursor = lmdb::cursor::open(txn, self.dbi.omemo);

    std::string_view k, v;
    // position cursor at first key >= prefix
    k = prefix;
    v = {};
    const bool found = cursor.get(k, v, MDB_SET_RANGE);
    if (!found)
        return omemo_trust::BLIND;

    do {
        if (!k.starts_with(prefix))
            break;
        const auto parsed = parse_uint32(v);
        if (parsed)
        {
            const auto t = static_cast<omemo_trust>(*parsed);
            if (t == omemo_trust::VERIFIED || t == omemo_trust::UNTRUSTED)
                return omemo_trust::UNDECIDED;
        }
    } while (cursor.get(k, v, MDB_NEXT));

    return omemo_trust::BLIND;
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

[[nodiscard]] auto base64_encode_raw(std::span<const std::uint8_t> data) -> std::string
{
    if (data.empty())
        return {};

    const int encoded_size = 4 * static_cast<int>((data.size() + 2) / 3) + 1;
    std::string encoded(static_cast<std::size_t>(encoded_size), '\0');
    const int written = weechat_string_base_encode("64",
        reinterpret_cast<const char *>(data.data()), static_cast<int>(data.size()), encoded.data());
    if (written <= 0)
        return {};
    encoded.resize(static_cast<std::size_t>(written));
    return encoded;
}

[[nodiscard]] auto base64_encode([[maybe_unused]] xmpp_ctx_t *context,
                                 std::span<const unsigned char> data) -> std::string
{
    return base64_encode_raw(data);
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

// Request the legacy OMEMO device list (eu.siacs.conversations.axolotl.devicelist) for jid.
void request_axolotl_devicelist(weechat::account &account, std::string_view jid)
{
    const std::string target_jid = normalize_bare_jid(account.context, jid);
    if (account.omemo.missing_axolotl_devicelist.contains(target_jid))
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

[[nodiscard]] auto pkcs7_pad(std::string_view plaintext, std::size_t block_size)
    -> std::vector<std::uint8_t>
{
    std::vector<std::uint8_t> output(plaintext.begin(), plaintext.end());
    const std::size_t actual_padding = (output.size() % block_size == 0) ? block_size : (block_size - (output.size() % block_size));
    output.insert(output.end(), actual_padding, static_cast<std::uint8_t>(actual_padding));
    return output;
}
