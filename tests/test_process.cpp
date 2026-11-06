#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "Utils/Process.h"

// Keep in sync with k_pszStashedOverlayPreload in Process.cpp.
static constexpr const char *k_pszStashedOverlayPreload = "GAMESCOPE_STEAM_OVERLAY_LD_PRELOAD";

static const char *k_pszOverlay = "/home/user/.local/share/Steam/ubuntu12_64/gameoverlayrenderer.so";

static std::string GetEnv( const char *pszName )
{
	const char *pszValue = getenv( pszName );
	REQUIRE( pszValue != nullptr );
	return pszValue;
}

TEST_CASE("RemoveSteamOverlayFromPreload", "[process]") {
	SECTION("does nothing when LD_PRELOAD is unset") {
		unsetenv( "LD_PRELOAD" );
		gamescope::Process::RemoveSteamOverlayFromPreload();
		REQUIRE( getenv( "LD_PRELOAD" ) == nullptr );
	}

	SECTION("unsets LD_PRELOAD when the overlay is the only entry") {
		setenv( "LD_PRELOAD", k_pszOverlay, 1 );
		gamescope::Process::RemoveSteamOverlayFromPreload();
		REQUIRE( getenv( "LD_PRELOAD" ) == nullptr );
	}

	SECTION("removes the overlay and keeps the other entries") {
		setenv( "LD_PRELOAD", ( std::string{ "libfirst.so:" } + k_pszOverlay + ":liblast.so" ).c_str(), 1 );
		gamescope::Process::RemoveSteamOverlayFromPreload();
		REQUIRE( GetEnv( "LD_PRELOAD" ) == "libfirst.so:liblast.so" );
	}

	SECTION("handles space-separated lists") {
		setenv( "LD_PRELOAD", ( std::string{ "libfirst.so " } + k_pszOverlay ).c_str(), 1 );
		gamescope::Process::RemoveSteamOverlayFromPreload();
		REQUIRE( GetEnv( "LD_PRELOAD" ) == "libfirst.so" );
	}

	SECTION("leaves the other entries alone when the overlay is absent") {
		setenv( "LD_PRELOAD", "libfirst.so:liblast.so", 1 );
		gamescope::Process::RemoveSteamOverlayFromPreload();
		REQUIRE( GetEnv( "LD_PRELOAD" ) == "libfirst.so:liblast.so" );
	}
}

TEST_CASE("RestoreSteamOverlayPreload", "[process]") {
	SECTION("reports when there is nothing stashed") {
		unsetenv( k_pszStashedOverlayPreload );
		setenv( "LD_PRELOAD", "libuntouched.so", 1 );
		REQUIRE( gamescope::Process::RestoreSteamOverlayPreload() == false );
		REQUIRE( GetEnv( "LD_PRELOAD" ) == "libuntouched.so" );
	}

	SECTION("puts the stashed preload back and consumes the stash") {
		setenv( k_pszStashedOverlayPreload, k_pszOverlay, 1 );
		unsetenv( "LD_PRELOAD" );
		REQUIRE( gamescope::Process::RestoreSteamOverlayPreload() == true );
		REQUIRE( GetEnv( "LD_PRELOAD" ) == k_pszOverlay );
		REQUIRE( getenv( k_pszStashedOverlayPreload ) == nullptr );
	}
}

TEST_CASE("GetAppIdFromCgroup", "[process]") {
    SECTION("steam app scope") {
        std::istringstream stream("0::/user.slice/user-1000.slice/user@1000.service/app.slice/app-steam-app12345-9876.scope\n");
        REQUIRE(gamescope::Process::GetAppIdFromCgroup(stream) == 12345u);
    }

    SECTION("no matching scope") {
        std::istringstream stream("0::/user.slice/user-1000.slice/session.scope\n");
        REQUIRE(gamescope::Process::GetAppIdFromCgroup(stream) == 0u);
    }

    SECTION("multiple lines, one matching") {
        std::istringstream stream(
            "12:devices:/user.slice\n"
            "11:memory:/user.slice/user-1000.slice/session-1.scope\n"
            "0::/user.slice/user-1000.slice/user@1000.service/app.slice/app-steam-app567-42.scope\n"
        );
        REQUIRE(gamescope::Process::GetAppIdFromCgroup(stream) == 567u);
    }

    SECTION("empty stream") {
        std::istringstream stream("");
        REQUIRE(gamescope::Process::GetAppIdFromCgroup(stream) == 0u);
    }

    SECTION("malformed lines") {
        std::istringstream stream("not-a-valid-cgroup-line\n");
        REQUIRE(gamescope::Process::GetAppIdFromCgroup(stream) == 0u);

        stream = std::istringstream("not-a-valid:cgroup-line\n");
        REQUIRE(gamescope::Process::GetAppIdFromCgroup(stream) == 0u);

        stream = std::istringstream("not-a-valid:cgroup-line:\n");
        REQUIRE(gamescope::Process::GetAppIdFromCgroup(stream) == 0u);
    }
}

namespace {

struct ScopedReaper {
    pid_t reaper_pid = -1;
    pid_t game_pid   = -1;

    static std::string HelperPath() {
        char buf[PATH_MAX];
        ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (len < 0) return "";
        buf[len] = '\0';
        return std::filesystem::path(buf).parent_path() / "reaper_test_helper";
    }

    // extra_args appear in /proc/reaper_pid/cmdline after the binary name
    static ScopedReaper Spawn(std::vector<std::string> extra_args) {
        ScopedReaper reaper;
        int pipefd[2];
        if (pipe(pipefd) != 0) return reaper;

        pid_t pid = fork();
        if (pid < 0)
        {
            close(pipefd[0]);
            close(pipefd[1]);
            return reaper;
        }

        if (pid == 0)
        {
            close(pipefd[0]);
            dup2(pipefd[1], STDOUT_FILENO);
            close(pipefd[1]);

            std::string helper = HelperPath();
            std::vector<char *> argv;
            argv.push_back(const_cast<char *>(helper.c_str()));
            for (auto &arg : extra_args)
                argv.push_back(const_cast<char *>(arg.c_str()));
            argv.push_back(nullptr);
            execvp(argv[0], argv.data());
            _exit(1);
        }

        close(pipefd[1]);
        reaper.reaper_pid = pid;

        char buf[32] = {};
        read(pipefd[0], buf, sizeof(buf) - 1);
        close(pipefd[0]);
        reaper.game_pid = atoi(buf);
        return reaper;
    }

    ~ScopedReaper() {
        if (game_pid > 0)
        {
            kill(game_pid, SIGKILL);
            waitpid(game_pid, nullptr, 0);
        }
        if (reaper_pid > 0)
        {
            kill(reaper_pid, SIGKILL);
            waitpid(reaper_pid, nullptr, 0);
        }
    }
};

}

TEST_CASE("GetAppIdFromReaper", "[process]") {
    SECTION("well-formed reaper command line") {
        auto reaper = ScopedReaper::Spawn({"SteamLaunch", "AppId=42", "--", "game"});
        REQUIRE(reaper.game_pid > 0);
        REQUIRE(gamescope::Process::GetAppIdFromReaper(reaper.game_pid) == 42);
    }

    SECTION("AppId must come after SteamLaunch") {
        auto reaper = ScopedReaper::Spawn({"AppId=42", "SteamLaunch", "--", "game"});
        REQUIRE(reaper.game_pid > 0);
        REQUIRE(gamescope::Process::GetAppIdFromReaper(reaper.game_pid) == 0u);
    }

    SECTION("missing SteamLaunch token") {
        auto reaper = ScopedReaper::Spawn({"AppId=42", "--", "game"});
        REQUIRE(reaper.game_pid > 0);
        REQUIRE(gamescope::Process::GetAppIdFromReaper(reaper.game_pid) == 0u);
    }

    SECTION("-- stops parsing before AppId is seen") {
        auto reaper = ScopedReaper::Spawn({"SteamLaunch", "--", "AppId=42", "game"});
        REQUIRE(reaper.game_pid > 0);
        REQUIRE(gamescope::Process::GetAppIdFromReaper(reaper.game_pid) == 0u);
    }

    SECTION("AppId=0 is excluded") {
        auto reaper = ScopedReaper::Spawn({"SteamLaunch", "AppId=0", "--", "game"});
        REQUIRE(reaper.game_pid > 0);
        REQUIRE(gamescope::Process::GetAppIdFromReaper(reaper.game_pid) == 0u);
    }

    SECTION("malformed AppId is ignored") {
        auto reaper = ScopedReaper::Spawn({"SteamLaunch", "AppId=foobar", "--", "game"});
        REQUIRE(reaper.game_pid > 0);
        REQUIRE(gamescope::Process::GetAppIdFromReaper(reaper.game_pid) == 0u);
    }

    SECTION("no reaper in the process tree") {
        REQUIRE(gamescope::Process::GetAppIdFromReaper(getpid()) == 0u);
    }

    SECTION("non-existent pid") {
        REQUIRE(gamescope::Process::GetAppIdFromReaper(std::numeric_limits<pid_t>::max()) == 0u);
    }
}

static int WaitStatusOf( void (*fnChild)() )
{
	pid_t nPid = fork();
	REQUIRE( nPid >= 0 );
	if ( nPid == 0 )
		fnChild();

	int nStatus = 0;
	REQUIRE( gamescope::Process::WaitForAllChildren( nPid, &nStatus ) );
	return nStatus;
}

TEST_CASE("ExitCodeFromWaitStatus", "[process]") {
	SECTION("exit code is passed through") {
		REQUIRE( gamescope::Process::ExitCodeFromWaitStatus( WaitStatusOf( []{ _exit( 0 ); } ) ) == 0 );
		REQUIRE( gamescope::Process::ExitCodeFromWaitStatus( WaitStatusOf( []{ _exit( 7 ); } ) ) == 7 );
	}
	SECTION("killed by a signal gives 128 + the signal") {
		REQUIRE( gamescope::Process::ExitCodeFromWaitStatus( WaitStatusOf( []{ signal( SIGTERM, SIG_DFL ); raise( SIGTERM ); _exit( 0 ); } ) ) == 128 + SIGTERM );
		REQUIRE( gamescope::Process::ExitCodeFromWaitStatus( WaitStatusOf( []{ raise( SIGKILL ); _exit( 0 ); } ) ) == 128 + SIGKILL );
	}
}

static int RunReaper( const char *pszCommand, bool bTerminateReaper = false )
{
	// tests/meson.build passes the built reaper; fall back to its place in the build tree.
	std::string sReaper;
	if ( const char *pszReaper = getenv( "GAMESCOPEREAPER" ) )
		sReaper = pszReaper;
	else
	{
		char buf[PATH_MAX];
		ssize_t len = readlink( "/proc/self/exe", buf, sizeof( buf ) - 1 );
		REQUIRE( len > 0 );
		buf[len] = '\0';
		sReaper = std::filesystem::path( buf ).parent_path().parent_path() / "src" / "gamescopereaper";
	}

	pid_t nPid = fork();
	REQUIRE( nPid >= 0 );
	if ( nPid == 0 )
	{
		if ( pszCommand )
			execl( sReaper.c_str(), "gamescopereaper", "--", "sh", "-c", pszCommand, (char *)nullptr );
		else
			execl( sReaper.c_str(), "gamescopereaper", "--", "/nonexistent/gamescope-test-command", (char *)nullptr );
		_exit( 255 );
	}

	if ( bTerminateReaper )
	{
		// Wait until the child has exec'd `sleep`: before exec it still has the reaper's
		// SIGTERM handler, which would swallow the SIGTERM the reaper forwards.
		std::string sChildren = "/proc/" + std::to_string( nPid ) + "/task/" + std::to_string( nPid ) + "/children";
		bool bChildRunning = false;
		for ( int i = 0; i < 500 && !bChildRunning; i++ )
		{
			pid_t nChild = 0;
			if ( FILE *pChildren = fopen( sChildren.c_str(), "r" ) )
			{
				if ( fscanf( pChildren, "%d", &nChild ) != 1 )
					nChild = 0;
				fclose( pChildren );
			}
			char szComm[32] = {};
			if ( nChild > 0 )
			{
				std::string sComm = "/proc/" + std::to_string( nChild ) + "/comm";
				if ( FILE *pComm = fopen( sComm.c_str(), "r" ) )
				{
					if ( fgets( szComm, sizeof( szComm ), pComm ) )
						bChildRunning = !strcmp( szComm, "sleep\n" );
					fclose( pComm );
				}
			}
			if ( !bChildRunning )
				usleep( 10 * 1000 );
		}
		if ( !bChildRunning )
		{
			kill( nPid, SIGKILL );
			waitpid( nPid, nullptr, 0 );
		}
		REQUIRE( bChildRunning );
		kill( nPid, SIGTERM );
	}

	int nStatus = 0;
	REQUIRE( waitpid( nPid, &nStatus, 0 ) == nPid );
	REQUIRE( WIFEXITED( nStatus ) );
	return WEXITSTATUS( nStatus );
}

TEST_CASE("gamescopereaper exits with its primary child's status", "[process]") {
	REQUIRE( RunReaper( "exit 0" ) == 0 );
	REQUIRE( RunReaper( "exit 7" ) == 7 );
	REQUIRE( RunReaper( "kill -TERM $$" ) == 128 + SIGTERM );
	REQUIRE( RunReaper( "kill -KILL $$" ) == 128 + SIGKILL );
	// A command that can't be started gives 127, like a shell.
	REQUIRE( RunReaper( nullptr ) == 127 );
	// Terminating the reaper terminates the child, and the reaper reports how it died.
	REQUIRE( RunReaper( "exec sleep 30", true ) == 128 + SIGTERM );
}
