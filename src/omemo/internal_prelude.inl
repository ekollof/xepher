
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

[[nodiscard]] auto normalize_bare_jid(xmpp_ctx_t *context, std::string_view jid) -> std::string
{
    if (!context)
        return std::string {jid};

    xmpp_string_guard bare_g(context, xmpp_jid_bare(context, std::string {jid}.c_str()));
    if (bare_g.ptr && *bare_g.ptr)
        return bare_g.ptr;
    return std::string {jid};
}

void request_devicelist(weechat::account &account, std::string_view jid)
{
    const std::string target_jid = normalize_bare_jid(account.context, jid);
    if (account.omemo.missing_omemo2_devicelist.count(target_jid) != 0)
        return;

    xmpp_stanza_t *children[2] = {nullptr, nullptr};
    children[0] = stanza__iq_pubsub_items(*account.context, nullptr, kDevicesNode.data());
    children[0] = stanza__iq_pubsub(*account.context, nullptr, children,
                                    with_noop("http://jabber.org/protocol/pubsub"));

    xmpp_string_guard uuid_g(*account.context, xmpp_uuid_gen(*account.context));
    const char *uuid = uuid_g.ptr;
    children[0] = stanza__iq(*account.context, nullptr, children, nullptr, uuid,
                             account.jid().data(), target_jid.c_str(), "get");
    if (uuid)
        account.omemo.pending_iq_jid[uuid] = target_jid;

    account.connection.send(children[0]);
    xmpp_stanza_release(children[0]);
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
    xmpp_stanza_t *item_stanza =
        stanza__iq_pubsub_items_item(*account.context, nullptr, with_noop(item_id.c_str()));
    xmpp_stanza_t *items_stanza =
        stanza__iq_pubsub_items(*account.context, nullptr, kBundlesNode.data());
    xmpp_stanza_add_child(items_stanza, item_stanza);
    xmpp_stanza_release(item_stanza);
    xmpp_stanza_t *pubsub_children[] = {items_stanza, nullptr};
    xmpp_stanza_t *pubsub_stanza =
        stanza__iq_pubsub(*account.context, nullptr, pubsub_children,
                          with_noop("http://jabber.org/protocol/pubsub"));

    xmpp_string_guard uuid_g(*account.context, xmpp_uuid_gen(*account.context));
    const char *uuid = uuid_g.ptr;
    xmpp_stanza_t *iq_children[] = {pubsub_stanza, nullptr};
    xmpp_stanza_t *iq_stanza =
        stanza__iq(*account.context, nullptr, iq_children, nullptr, uuid,
                   account.jid().data(), target_jid.c_str(), "get");
    if (uuid)
        account.omemo.pending_iq_jid[uuid] = target_jid;
    account.omemo.pending_bundle_fetch.insert(key);

    XDEBUG("omemo: sent bundle request for {}/{} (uuid={})",
           target_jid, device_id, uuid ? uuid : "?");

    account.connection.send(iq_stanza);
    xmpp_stanza_release(iq_stanza);
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

[[nodiscard]] auto key_for_devicelist(std::string_view jid) -> std::string
{
    return fmt::format("devicelist:{}", jid);
}

[[nodiscard]] auto key_for_bundle(std::string_view jid, std::uint32_t device_id) -> std::string
{
    return fmt::format("bundle:{}:{}", jid, device_id);
}

// Legacy OMEMO uses separate LMDB keys so we can distinguish which protocol
// version a peer advertises (they may have both, though unlikely).
[[nodiscard]] auto key_for_legacy_devicelist(std::string_view jid) -> std::string
{
    return fmt::format("legacy_devicelist:{}", jid);
}

// Legacy bundles share the same Signal session store as OMEMO:2 bundles — the
// Signal crypto is identical.  Only the XMPP stanza wrapping differs.
// We use a separate LMDB key prefix so we can tell which namespace the bundle
// came from when deciding which encode path to use.
[[nodiscard]] auto key_for_legacy_bundle(std::string_view jid, std::uint32_t device_id) -> std::string
{
    return fmt::format("legacy_bundle:{}:{}", jid, device_id);
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
    else if (mode == omemo::peer_mode::legacy)
        mode_value = "legacy";
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
    if (mode_value == "legacy")
        return omemo::peer_mode::legacy;

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

    xmpp_string_guard text {xmpp_stanza_get_context(stanza), xmpp_stanza_get_text(stanza)};
    return text ? text.str() : std::string {};
}

[[nodiscard]] auto base64_encode(xmpp_ctx_t *context, const unsigned char *data, std::size_t size)
    -> std::string
{
    xmpp_string_guard encoded {context, xmpp_base64_encode(context, data, size)};
    return encoded ? encoded.str() : std::string {};
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

[[nodiscard]] auto base64_decode(xmpp_ctx_t *context, std::string_view encoded)
    -> std::vector<std::uint8_t>
{
    unsigned char *decoded = nullptr;
    size_t decoded_size = 0;
    xmpp_base64_decode_bin(context, encoded.data(), encoded.size(), &decoded, &decoded_size);
    if (!decoded || decoded_size == 0)
        return {};

    std::vector<std::uint8_t> result(decoded, decoded + decoded_size);
    xmpp_free(context, decoded);
    return result;
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
void request_legacy_devicelist(weechat::account &account, std::string_view jid)
{
    const std::string target_jid = normalize_bare_jid(account.context, jid);
    if (account.omemo.missing_legacy_devicelist.count(target_jid) != 0)
        return;

    xmpp_stanza_t *children[2] = {nullptr, nullptr};
    children[0] = stanza__iq_pubsub_items(*account.context, nullptr, kLegacyDevicesNode.data());
    children[0] = stanza__iq_pubsub(*account.context, nullptr, children,
                                    with_noop("http://jabber.org/protocol/pubsub"));

    xmpp_string_guard uuid_g(*account.context, xmpp_uuid_gen(*account.context));
    const char *uuid = uuid_g.ptr;
    children[0] = stanza__iq(*account.context, nullptr, children, nullptr, uuid,
                             account.jid().data(), target_jid.c_str(), "get");
    if (uuid)
        account.omemo.pending_iq_jid[uuid] = target_jid;

    XDEBUG("omemo: requesting legacy device list for {}", target_jid);

    account.connection.send(children[0]);
    xmpp_stanza_release(children[0]);
}

// Request a legacy OMEMO bundle (eu.siacs.conversations.axolotl.bundles:{device_id}) for jid.
// Legacy bundles use a per-device node (device ID embedded in node name, not item ID).
void request_legacy_bundle(weechat::account &account, std::string_view jid, std::uint32_t device_id)
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
    xmpp_stanza_t *items_stanza =
        stanza__iq_pubsub_items(*account.context, nullptr, bundle_node.c_str());
    xmpp_stanza_t *pubsub_children[] = {items_stanza, nullptr};
    xmpp_stanza_t *pubsub_stanza =
        stanza__iq_pubsub(*account.context, nullptr, pubsub_children,
                          with_noop("http://jabber.org/protocol/pubsub"));

    xmpp_string_guard uuid_g(*account.context, xmpp_uuid_gen(*account.context));
    const char *uuid = uuid_g.ptr;
    xmpp_stanza_t *iq_children[] = {pubsub_stanza, nullptr};
    xmpp_stanza_t *iq_stanza =
        stanza__iq(*account.context, nullptr, iq_children, nullptr, uuid,
                   account.jid().data(), target_jid.c_str(), "get");
    if (uuid)
        account.omemo.pending_iq_jid[uuid] = target_jid;
    account.omemo.pending_bundle_fetch.insert(key);

    XDEBUG("omemo: requesting legacy bundle for {}/{} (node={})",
           target_jid, device_id, bundle_node);

    account.connection.send(iq_stanza);
    xmpp_stanza_release(iq_stanza);
}

[[nodiscard]] auto sce_wrap(xmpp_ctx_t *context, weechat::account &account,
                            std::string_view plaintext) -> std::string
{
    const char *bound = xmpp_conn_get_bound_jid(account.connection);
    xmpp_string_guard bare_guard {context, bound ? xmpp_jid_bare(context, bound) : nullptr};
    const std::string from = bare_guard ? bare_guard.str() : std::string {};

    return fmt::format(
        "<envelope xmlns='urn:xmpp:sce:1'><content><body xmlns='jabber:client'>{}</body></content><time stamp='{}'/><from jid='{}'/><rpad/></envelope>",
        xml_escape(plaintext),
        utc_timestamp_now(),
        xml_escape(from));
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
