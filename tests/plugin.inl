#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include <iterator>
#include <regex>
#include <exception>
#include <filesystem>
#include <sys/types.h>
#include <sys/wait.h>
#include <weechat/weechat-plugin.h>
#include "../deps/fdstream.hpp"
#include "../src/plugin.hh"

std::vector<std::string> weechat_read_lines(std::vector<std::string_view> commands, bool echo = false) {
    std::ostringstream args;
    for (std::string_view arg : commands) {
        args << arg << ';'; 
    }

    std::string invocation("timeout 5 weechat-headless --stdout -a -t -P buflist,relay,python -r '" + args.str() + "/quit'");
    FILE *pipe = popen(invocation.data(), "r");
    CHECK(pipe != nullptr);

    boost::fdistream pipestream(fileno(pipe));
    std::vector<std::string> output;
    for (std::string line; std::getline(pipestream, line); line.clear()) {
        output.push_back(line);
    }

    int exitcode = 0;
    SUBCASE("unloads successfully")
    {
        /* thread-unsafe inside block */ {
            int status = pclose(pipe);
            if (WIFEXITED(status))
                exitcode = WEXITSTATUS(status);
            else if (WIFSIGNALED(status)) {
                const char *signal = strsignal(WTERMSIG(status));
                throw std::runtime_error(signal);
            }
        }

        CHECK(exitcode == 0);
    }

    CHECK(output.size() > 0);
    
    if (echo) {
        std::cerr << "weechat (" << exitcode << "): " << args.str() << std::endl;
        for (std::string line : output) {
            std::cerr << line << std::endl;
        }
    }

    return output;
}

bool weechat_headless_available()
{
    const char *path_env = std::getenv("PATH");
    if (!path_env)
        return false;

    std::string_view path(path_env);
    std::size_t start = 0;
    while (start <= path.size())
    {
        const std::size_t end = path.find(':', start);
        const std::string_view segment = path.substr(start, end == std::string_view::npos
                                                            ? path.size() - start
                                                            : end - start);
        if (!segment.empty())
        {
            std::filesystem::path candidate(segment);
            candidate /= "weechat-headless";
            if (std::filesystem::exists(candidate))
                return true;
        }

        if (end == std::string_view::npos)
            break;
        start = end + 1;
    }

    return false;
}

TEST_CASE("weechat" * doctest::may_fail())
{
    const std::string plugin_api_version(WEECHAT_PLUGIN_API_VERSION);

    SUBCASE("plugin api match")
    {
        CHECK(plugin_api_version == WEECHAT_PLUGIN_API_VERSION);
    }

    SUBCASE("launches")
    {
        if (!weechat_headless_available())
        {
            MESSAGE("Skipping weechat-headless launch test: binary not available in PATH");
            return;
        }

        std::vector<std::string> output = weechat_read_lines({
                "/print -stdout -escape TEST_OK\\n",
                }, true);

        bool recieved_ok = std::find_if(output.begin(), output.end(), [](std::string& line){
                    return line.find("TEST_OK") != std::string::npos;
                    }) != output.end();
        CHECK(recieved_ok);
    }

    SUBCASE("plugin loads")
    {
        if (!weechat_headless_available())
        {
            MESSAGE("Skipping weechat-headless plugin-load test: binary not available in PATH");
            return;
        }

        SUBCASE("without incursion")
        {
            const std::regex line_pattern("\\[[^\\]]*\\]\\s*line \\d*: (.*)");

            std::vector<std::string> output = weechat_read_lines({
                    "/plugin load ../xmpp.so",
                    "/print -stdout -escape TEST_BufferStart\\n",
                    "/debug buffer",
                    "/print -stdout -escape TEST_BufferEnd\\n",
                    "/plugin listfull",
                    }, false);

            std::vector<std::string> buffer;

            auto it = output.begin();
            while (it != output.end() && it->find("TEST_BufferStart") != std::string::npos) {
                it++;
            }

            while (it != output.end() && it->find("TEST_BufferEnd") == std::string::npos) {
                std::string& line = *it++;

                std::smatch match;
                if (std::regex_match(line, match, line_pattern, std::regex_constants::match_default)) {
                    buffer.push_back(match[1]);
                }
            }

            for (std::string line : buffer) {
                std::cerr << line << std::endl;
            }

            bool plugin_loaded = std::find_if(buffer.begin(), buffer.end(), [](std::string& line){
                        return line.find("Plugin \"xmpp\" loaded") != std::string::npos;
                        }) != buffer.end();
            CHECK(plugin_loaded);
        }
    }
}
