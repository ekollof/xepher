// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "data_form.hh"

#include <algorithm>
#include <array>
#include <ranges>
#include <set>
#include <fmt/core.h>

namespace xmpp {

namespace {
constexpr std::string_view data_ns = "jabber:x:data";

auto read_fields(StanzaView parent) -> std::expected<std::vector<data_form_field>, std::string>
{
    std::vector<data_form_field> fields;
    std::set<std::string> names;
    for (const auto child : parent) {
        if (child.name() != "field" || child.xmlns() != data_ns) continue;
        data_form_field field;
        field.var = child.attr_string("var");
        field.label = child.attr_string("label");
        field.type = child.attr_string("type");
        constexpr std::array<std::string_view, 9> types{"boolean", "fixed", "hidden",
            "jid-multi", "jid-single", "list-multi", "list-single", "text-multi", "text-private"};
        if (!std::ranges::contains(types, field.type)) field.type = "text-single";
        if (field.type != "fixed" && (field.var.empty() || !names.insert(field.var).second))
            return std::unexpected("Form contains a missing or duplicate field name");
        field.required = child.child("required", data_ns).valid();
        field.description = child.child("desc", data_ns).text();
        std::ranges::for_each(child, [&](StanzaView node) {
            if (node.xmlns() != data_ns) return;
            if (node.name() == "value") field.values.emplace_back(node.text());
            if (node.name() == "option") {
                const auto value = node.child("value", data_ns);
                if (value.valid()) field.options.push_back({node.attr_string("label"), value.text()});
            }
        });
        field.included = !field.values.empty() || field.type == "hidden";
        if (field.type == "boolean" && field.values.empty()) {
            field.values.emplace_back("0");
            field.included = true;
        }
        fields.emplace_back(std::move(field));
    }
    return fields;
}

auto checked_values(const data_form_field &field, std::span<const std::string> values)
    -> std::expected<std::vector<std::string>, std::string>
{
    const bool multi = field.type == "hidden" || field.type.ends_with("-multi");
    if (!multi && values.size() > 1)
        return std::unexpected("This field accepts only one value");
    if (field.required && (values.empty() || std::ranges::all_of(values, &std::string::empty)))
        return std::unexpected("A value is required");
    std::vector<std::string> result(values.begin(), values.end());
    if (field.type == "boolean" && !result.empty()) {
        auto &value = result.front();
        if (value == "true" || value == "1") value = "1";
        else if (value == "false" || value == "0") value = "0";
        else return std::unexpected("Use true, false, 1, or 0");
    }
    if (field.type.starts_with("list-")) {
        if (std::ranges::any_of(result, [&](std::string_view value) {
                return !std::ranges::contains(field.options, value, &data_form_option::value);
            }))
            return std::unexpected("Choose only the offered options");
        // XEP-0004 requires list-multi selections to retain server option order.
        result = field.options | std::views::filter([&](const auto &option) {
            return std::ranges::contains(values, option.value);
        }) | std::views::transform(&data_form_option::value) | std::ranges::to<std::vector>();
    }
    if (field.type.starts_with("jid-")) {
        if (std::ranges::any_of(result, [](std::string_view value) {
                const auto bare = value.substr(0, value.find('/'));
                const auto at = bare.find('@');
                const auto local = at == std::string_view::npos ? std::string_view{} : bare.substr(0, at);
                const auto domain = bare.substr(at == std::string_view::npos ? 0 : at + 1);
                const auto slash = value.find('/');
                return bare.empty() || bare.ends_with('@') || at == 0
                    || local.size() > 1023 || domain.size() > 1023
                    || (slash != std::string_view::npos && (slash + 1 == value.size() || value.size() - slash - 1 > 1023))
                    || std::ranges::any_of(local, [](char c) { return std::string_view("\"&':<>@").contains(c); })
                    || (at != std::string_view::npos && bare.substr(at + 1).contains('@'))
                    || std::ranges::any_of(value, [](unsigned char c) { return c < 32 || c == 127; })
                    || std::ranges::any_of(bare, [](unsigned char c) { return c <= 32 || c == 127; });
            }))
            return std::unexpected("Enter a valid JID");
        std::vector<std::string> unique;
        std::ranges::for_each(result, [&](const auto &value) {
            if (!std::ranges::contains(unique, value)) unique.emplace_back(value);
        });
        result = std::move(unique);
    }
    return result;
}
}

auto parse_data_form(StanzaView view) -> std::expected<data_form, std::string>
{
    if (!view.valid() || view.name() != "x" || view.xmlns() != data_ns)
        return std::unexpected("Not a data form");
    data_form form;
    form.type = view.attr_string("type");
    if (form.type != "form" && form.type != "result")
        return std::unexpected("Expected a form or result");
    form.title = view.child("title", data_ns).text();
    std::ranges::for_each(view, [&](StanzaView node) {
        if (node.name() == "instructions" && node.xmlns() == data_ns)
            form.instructions.emplace_back(node.text());
    });
    auto fields = read_fields(view);
    if (!fields) return std::unexpected(fields.error());
    form.fields = std::move(*fields);
    const auto reported = view.child("reported", data_ns);
    if (reported.valid()) {
        auto columns = read_fields(reported);
        if (!columns) return std::unexpected(columns.error());
        form.reported = std::move(*columns);
        for (const auto item : view) {
            if (item.name() != "item" || item.xmlns() != data_ns) continue;
            auto row = read_fields(item);
            if (!row) return std::unexpected(row.error());
            form.items.emplace_back(std::move(*row));
        }
    }
    return form;
}

auto set_data_form_values(data_form_field &field, std::span<const std::string> values)
    -> std::expected<void, std::string>
{
    if (!field.editable()) return std::unexpected("This field is read-only");
    auto checked = checked_values(field, values);
    if (!checked) return std::unexpected(checked.error());
    field.values = std::move(*checked);
    field.included = true;
    return {};
}

auto submit_data_form(const data_form &form) -> std::expected<stanza::xep0004::form, std::string>
{
    if (form.type != "form") return std::unexpected("Only input forms can be submitted");
    stanza::xep0004::form submission("submit");
    for (const auto &field : form.fields) {
        if (field.type == "fixed") continue;
        if (!field.included && !field.required) continue;
        auto checked = checked_values(field, field.values);
        if (!checked) return std::unexpected(fmt::format("{}: {}", field.label.empty() ? field.var : field.label, checked.error()));
        stanza::xep0004::field out(field.var);
        out.type(field.type);
        std::ranges::for_each(*checked, [&](std::string_view value) { out.value(value); });
        submission.add_field(out);
    }
    return submission;
}

auto data_form_lines(const data_form &form) -> std::vector<std::string>
{
    std::vector<std::string> lines;
    if (!form.title.empty()) lines.emplace_back(form.title);
    std::ranges::copy(form.instructions, std::back_inserter(lines));
    auto describe = [&](const data_form_field &field, std::size_t number) {
        if (field.type == "hidden") return;
        const auto values = field.type == "text-private" ? std::string("********")
            : field.values | std::views::join_with(std::string_view(" | ")) | std::ranges::to<std::string>();
        if (field.type == "fixed") { lines.emplace_back(values); return; }
        lines.emplace_back(fmt::format("{}. {}{} [{}] = {}", number,
            field.label.empty() ? field.var : field.label, field.required ? " *" : "", field.type,
            !field.included ? "(omitted)" : values.empty() ? "(empty)" : values));
    };
    std::ranges::for_each(form.fields | std::views::enumerate, [&](const auto entry) {
        const auto &[index, field] = entry;
        describe(field, index + 1);
    });
    std::ranges::for_each(form.items | std::views::enumerate, [&](const auto entry) {
        const auto &[index, row] = entry;
        lines.emplace_back(fmt::format("Result {}:", index + 1));
        std::ranges::for_each(form.reported, [&](const auto &column) {
            const auto value = std::ranges::find(row, column.var, &data_form_field::var);
            if (value == row.end()) return;
            auto field = *value;
            field.type = column.type;
            field.label = column.label;
            describe(field, 0);
        });
    });
    return lines;
}

std::vector<DataFormFieldInfo> parse_data_form_fields(StanzaView xdata_form)
{
    if (!xdata_form.valid())
        return {};

    std::vector<DataFormFieldInfo> fields;
    for (const auto field : xdata_form)
    {
        if (field.name() != "field")
            continue;
        const std::string var = field.attr_string("var");
        if (var.empty() || var == "FORM_TYPE")
            continue;
        DataFormFieldInfo parsed;
        parsed.var = var;
        parsed.label = field.attr_string("label");
        fields.push_back(std::move(parsed));
    }
    return fields;
}

}  // namespace xmpp
