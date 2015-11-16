# 소개 #

  * windows 환경에서 토렌트파일을 읽고 다운로드 하거나 .torrent 파일을 만들 수 있는 dll 라이브러리 솔루션입니다.
  * 토렌트 파일을 제어하기위한 기본 라이브러리로 libtorrent ( http://www.libtorrent.org ) 1.06 를 사용하였습니다.
  * 컴파일을 위해 boost ( 1.46.1 http://www.boost.org )와 openssl library ( http://www.openssl.org/ )가 필요합니다.


# 사용법 #

  * 헤더파일과 lib 파일을 프로젝트로 포함시킨 후 dll 파일을 작업 디렉토리로 복사합니다.

  * 아래는 간단한 사용 예제입니다.

토렌파일 만들기 :
```
#include "bittorrent.h"
#pragma comment (lib, "bittorrent.lib")

int _tmain(int argc, _TCHAR* argv[])
{	
	std::vector<std::string> webseeds;
	std::vector<std::string> trackers;

	// if you have a web seed, add your web seed
	webseeds.push_back( "http://dl.dropbox.com/s/qod5i7lsqo2r9hb/20120723_145231.mp4" );

	// add trackers
	trackers.push_back( "udp://tracker.openbittorrent.com:80/announce" );
	trackers.push_back( "udp://tracker.publicbt.com:80/announce" );

	libtorrent::make( "20120723_145231.mp4", "test_new.torrent", [](int i,char const*msg){ ::MessageBoxA( 0, msg, 0, 0 ); }, [](int c, int m){}, webseeds, trackers );
	
	return 0;
}
```

토렌트 다운로드 추가/삭제 :
```
	// 추가
	// magnet, filename, http link, everything
	torrent_session.add( "http://dl.dropbox.com/s/tidb3j9pdwmhvr2/test3.torrent" );
	torrent_session.add( "test.torrent" );
	torrent_session.add( "magnet:?xt=urn:btih:E578B9873C12C393C2B2B07E21668C1498D88CA4&dn=%eb%82%98%eb%8a%94%20%ea%bc%bc%ec%88%98%eb%8b%a4%20-%20%eb%b4%89%ec%a3%bc17%ed%9a%8c.mp3&tr=udp%3a//tracker.openbittorrent.com%3a80/announce" );

	//삭제
	torrent_session.del( "test.torrent" );
```

멈추기와 재시작 :
```
	torrent_session.pause( torrent_tag );
	torrent_session.resume( torrent_tag );
```

magnet uri 만들기 :
```
	std::string magnet = torrent_session.get_magnet( torrent_tag );
```

metadata 얻기 :
```
	std::vector<char> meta_data;
	torrent_session.get_metadata( torrent_tag, meta_data );
```

토렌트 파일 다운로드 예제 :
```
#include "bittorrent.h"

#include <windows.h>
#include <iostream>

#pragma comment (lib, "bittorrent.lib")

int _tmain(int argc, _TCHAR* argv[])
{	
	bool finish = false;

	char const * test_torrent_name = "http://dl.dropbox.com/s/tidb3j9pdwmhvr2/test3.torrent";

	std::vector< std::string > setting_params;

	auto error_handler = [](int i, char const * msg){ 
		::MessageBoxA(0,msg, 0,0);
	};

	auto event_handler = [&finish,test_torrent_name]( libtorrent::eTorrentEvent e, std::string const & torrent ) 
	{
		switch( e )
		{
		case libtorrent::eTorrentEvent::torrent_event_add_failed :
			break;
		case libtorrent::eTorrentEvent::torrent_event_finished :
		{
			char msg[1024];
			sprintf_s(msg, sizeof(msg), "%s downdload is completed.\ndo you want exit?", torrent.c_str());
			if( ::MessageBoxA( 0, msg, "complete", MB_YESNO ) == S_OK )
			{
				finish = true;
			}
		}
			
			break;
		default:
			break;
		}
	};

	libtorrent::TorrentSession torrent_session( setting_params, error_handler, event_handler );

	torrent_session.set_print_debug( true );

	torrent_session.add( test_torrent_name );

	while(!finish)
	{
		Sleep(1);

		torrent_session.update();

		// if you need
		// std::string magnet = torrent_session.get_magnet( test_torrent_name );
		// torrent_session.pause( test_torrent_name );
		// torrent_session.resume( test_torrent_name );
		// std::vector<char> metadata;
		// torrent_session.get_metadata( test_torrent_name, metadata );
		// torrent_session.del( test_torrent_name );
	}	

	return 0;
}
```