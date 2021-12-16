#include "Connection.h"

#include <iostream>

#undef ERROR
#undef ERROR_INVALID_MESSAGE

namespace wsc
{
	std::vector<std::string> SplitBy(std::string const& str, const char delim)
	{
		size_t start;
		size_t end = 0;

		std::vector<std::string> res;
		while ((start = str.find_first_not_of(delim, end)) != std::string::npos)
		{
			end = str.find(delim, start);
			res.push_back(str.substr(start, end - start));
		}
		return res;
	}

	bool StartsWith(std::string str, std::string sw)
	{
		return str.rfind(sw, 0) == 0;
	}


	bool operator< (const Message& a, const Message& b)
	{
		return a.messageId < b.messageId;
	}

	bool IsValidMessage(String message)
	{
		for (auto& c : message)
		{
			if ((c >= '�' && c <= '�') || (c >= '�' && c <= '�') ||
				(c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
				(c >= '0' && c <= '9') ||
				c == ' ' || c == '-' || c == '_')
				continue;
			return false;
		}
		return true;
	}

	bool IsValidNickname(String nickname)
	{
		if (MAX_NICKNAME_SIZE != -1)
			return IsValidMessage(nickname);
		else
			return nickname.size() <= MAX_NICKNAME_SIZE && IsValidMessage(nickname);
	}


	const String SERVER_ERROR = "error: server error"; // any server error begins with this string

	// v client input errors v
	const String ERROR = "error: ";
	const String ERROR_UNKNOWN_COMMAND = "error: unknown command";
	const String ERROR_WRONG_STATE = "error: wrong state";
	const String ERROR_UNKNOWN_ID = "error: unknown id";
	const String ERROR_WRONG_OPPONENT_STATE = "error: opponent is not searching for the game";
	const String ERROR_INVALID_NICKNAME = "error: invalid nickname";
	const String ERROR_INVALID_MESSAGE = "error: invalid message";
	// ^ client input errors ^

	// v other messages v
	const String SUCCESS = "success";
	const String IN_GAME_WITH = "in game with:"; // + <opponent id>
	const String END_GAME = "end game";
	const String NEW_MESSAGE = "message:"; // + <message>
	const String LIST = "list:"; // + [<id>:<nickname>\n]...
	const String PLAYER_ID = "id:"; // + <id>
	const String NEW_OFFER = "offer:"; // + <id>
	// ^ other messages ^

	// v commands from the player v
	const String COMMAND_REGISTER = "register";    // data = <nickname>
	const String COMMAND_LIST = "list";
	const String COMMAND_MESSAGE = "message"; // data = <message>
	const String COMMAND_OFFER = "offer"; // data = <id>
	const String COMMAND_END_GAME = "end game";
	// ^ commands from the player ^

	void Connection::ParseServerMessage(std::string message)
	{
		if (StartsWith(message, PLAYER_ID))
		{
			id = message.substr(PLAYER_ID.size());
		}
		else if (StartsWith(message, LIST))
		{
			listResponse = message.substr(LIST.size());
			listUpdated = true;
		}
		else if (StartsWith(message, NEW_OFFER))
		{
			offers.push_back(message.substr(NEW_OFFER.size()));
		}
		else if (StartsWith(message, IN_GAME_WITH))
		{
			opponentID = message.substr(IN_GAME_WITH.size());
			state = State::InGame;

#ifdef _DEBUG
			parsedMessages.clear();
#endif
			unparsedMessages.clear();

			offers.clear();
		}
		else if (StartsWith(message, END_GAME))
		{
#ifdef _DEBUG
			parsedMessages.clear();
#endif
			unparsedMessages.clear();

			offers.clear();

			opponentID = PlayerID();

			state = State::Searching;
		}
		else if (StartsWith(message, NEW_MESSAGE))
		{
			MessageID messageID = GetMessageID();
			unparsedMessages.insert(std::make_pair(messageID, Message(opponentID, messageID, message.substr(NEW_MESSAGE.size()))));
		}
		else
		{
			throw UnhandledServerMessageException("Unrecognised server message: " + message);
		}
	}
	void Connection::ParseResponse(std::string id, std::string message)
	{
		MessageID messageID = std::stoi(id);
		responses[messageID] = message;
	}
	void Connection::ParseMessage(std::string message)
	{
		auto at = message.find('@');
		std::string id = message.substr(0, at);
		message = message.substr(at + 1);

		std::lock_guard<std::mutex> lock(dataMutex);
		if (id == "server")
			ParseServerMessage(message);
		else
			ParseResponse(id, message);
	}

	void Connection::MessageHandler(
		beast::error_code const& ec,		// Result of operation
		std::size_t bytes_written			// Number of bytes appended to buffer
	)
	{
		if (ec)
			return;

		std::string data = beast::buffers_to_string(buffer.data());
		buffer.clear();
		ParseMessage(data);

		ws.async_read(
			buffer,
			[&](beast::error_code const& ec, std::size_t bytes_written) {
				this->MessageHandler(ec, bytes_written);
			});
	}

	void Connection::RecieveMessages()
	{
		while (!stopRecieving)
		{
			ioc.poll();

			using namespace std::chrono_literals;
			std::this_thread::sleep_for(20ms);
		}
	}

	void Connection::Connect()
	{
		state = State::NotConnected;
		try
		{
			try
			{
				// Make the connection on the IP address we get from a lookup
				net::connect(ws.next_layer(), results.begin(), results.end());
			}
			catch (std::exception const& e)
			{
				std::string s = e.what();
				throw ConnectionException("Failed to connect to the server: " + s);
			}

			// Set a decorator to change the User-Agent of the handshake
			ws.set_option(websocket::stream_base::decorator(
				[](websocket::request_type& req)
				{
					req.set(http::field::user_agent,
						std::string(BOOST_BEAST_VERSION_STRING) +
						" websocket-client-coro");
				}));

			// Perform the websocket handshake
			ws.handshake(url, "/");

			ws.async_read(
				buffer,
				[&](beast::error_code const& ec, std::size_t bytes_written) {
					this->MessageHandler(ec, bytes_written);
				});

			state = State::Registration;

			recieverThread = std::thread([&]() { this->RecieveMessages(); });
		}
		catch (std::exception const& e)
		{
			throw ConnectionException(e.what());
		}
	}

	MessageID Connection::GetMessageID()
	{
		static MessageID nextMessageID = 0;
		return nextMessageID++;
	}

	void Connection::Send(String message)
	{
		MessageID messageID = GetMessageID();

		message = std::to_string(messageID) + '@' + message;

		try
		{
			ws.write(net::buffer(std::string(message)));
			String response;
			while (true)
			{
				dataMutex.lock();
				if (responses.count(messageID) > 0)
				{
					response = responses[messageID];
					responses.erase(messageID);
					break;
				}
				dataMutex.unlock();

				using namespace std::chrono_literals;
				std::this_thread::sleep_for(20ms);
			}

			if (StartsWith(response, ERROR))
			{
				String error = response.substr(ERROR.size());
				throw ConnectionException(error);
			}
		}
		catch (ConnectionException const& e)
		{
			throw e;
		}
		catch (std::exception const& e)
		{
			throw ConnectionException(e.what());
		}
	}
	void Connection::Send(String command, String data)
	{
		Send(command + ':' + data);
	}

	Connection::Connection() : url(DEFAULT_URL), port(DEFAULT_PORT), resolver(ioc), ws(ioc), results(resolver.resolve(url, port))
	{
		Connect();
	}
	Connection::Connection(std::string url, std::string port) : url(url), port(port), resolver(ioc), ws(ioc), results(resolver.resolve(url, port))
	{
		Connect();
	}
	Connection::~Connection()
	{
		stopRecieving = true;
		recieverThread.join();
	}

	Connection::State Connection::GetState()
	{
		return this->state;
	}

	void Connection::Register(String nickname)
	{
		if (!IsValidNickname(nickname))
			throw NicknameException("Invalid nickname");
		if (GetState() == State::NotConnected)
			throw StateException("Wrong state: connection to the server is not established");
		if (GetState() != State::Registration)
			throw StateException("Wrong state: player must not be registered");

		Send(COMMAND_REGISTER, nickname);
		state = State::Searching;
	}

	PlayerID Connection::GetID()
	{
		if (GetState() == State::NotConnected)
			throw StateException("Wrong state: connection to the server is not established");
		if (GetState() == State::Registration)
			throw StateException("Wrong state: player must be registered");

		return this->id;
	}

	std::vector<Player> Connection::GetPlayers()
	{
		if (GetState() == State::NotConnected)
			throw StateException("Wrong state: connection to the server is not established");
		if (GetState() != State::Searching)
			throw StateException("Wrong state: player must be in search for the game");
		
		listUpdated = false;
		Send(COMMAND_LIST);

		// might be a problem because listUpdated is accessed in another thread
		while (!listUpdated)
		{
			using namespace std::chrono_literals;
			std::this_thread::sleep_for(10ms);
		}
		
		auto players = SplitBy(listResponse, '\n');

		std::vector<Player> res;
		for (auto& s : players)
		{
			auto colon = s.find(':');
			PlayerID playerID = s.substr(0, colon);
			String playerNickname = s.substr(colon + 1);

			if (playerID != id)
				res.emplace_back(playerID, playerNickname);
		}
		return res;
	}

	void Connection::SendOffer(PlayerID sendTo)
	{
		if (GetState() == State::NotConnected)
			throw StateException("Wrong state: connection to the server is not established");
		if (GetState() != State::Searching)
			throw StateException("Wrong state: player must be in search for the game");

		Send(COMMAND_OFFER, sendTo);
	}
	std::vector<PlayerID> Connection::GetOffers()
	{
		if (GetState() == State::NotConnected)
			throw StateException("Wrong state: connection to the server is not established");
		if (GetState() != State::Searching)
			throw StateException("Wrong state: player must be in search for the game");

		return offers;
	}

	std::vector<Message> Connection::GetMessages()
	{
		if (GetState() == State::NotConnected)
			throw StateException("Wrong state: connection to the server is not established");
		if (GetState() != State::InGame)
			throw StateException("Wrong state: player must be in the game");

		std::vector<Message> res;
		res.reserve(unparsedMessages.size());

		dataMutex.lock();
		for (auto& p : unparsedMessages)
			res.push_back(p.second);
		dataMutex.unlock();

		return res;
	}
	void Connection::RemoveMessage(MessageID id)
	{
		if (GetState() == State::NotConnected)
			throw StateException("Wrong state: connection to the server is not established");
		if (GetState() != State::InGame)
			throw StateException("Wrong state: player must be in the game");

		std::lock_guard<std::mutex> lock(dataMutex);
		if (unparsedMessages.count(id) == 0)
			throw UnknownMessageIdException("Unknown message id: " + std::to_string(id));

#ifdef _DEBUG
		parsedMessages.insert(unparsedMessages.find(id)->second);
#endif
		unparsedMessages.erase(id);
	}

#ifdef _DEBUG
	std::vector<Message> Connection::GetParsedMessages()
	{
		if (GetState() == State::NotConnected)
			throw StateException("Wrong state: connection to the server is not established");
		if (GetState() != State::InGame)
			throw StateException("Wrong state: player must be in the game");

		std::lock_guard<std::mutex> lock(dataMutex);
		return std::vector<Message>(parsedMessages.begin(), parsedMessages.end());
	}

	std::vector<Message> Connection::GetAllMessages()
	{
		if (GetState() == State::NotConnected)
			throw StateException("Wrong state: connection to the server is not established");
		if (GetState() != State::InGame)
			throw StateException("Wrong state: player must be in the game");

		std::vector<Message> allMessages = GetMessages();
		std::vector<Message> parsedMessages = GetParsedMessages();
		allMessages.insert(allMessages.begin(), parsedMessages.begin(), parsedMessages.end());
		return allMessages;
	}
#endif

	void Connection::SendMessage(String message)
	{
		if (!IsValidMessage(message))
			throw MessageException("Invalid message");

		if (GetState() == State::NotConnected)
			throw StateException("Wrong state: connection to the server is not established");
		if (GetState() != State::InGame)
			throw StateException("Wrong state: player must be in the game");

		Send(COMMAND_MESSAGE, message);
	}

	void Connection::EndGame()
	{
		if (GetState() == State::NotConnected)
			throw StateException("Wrong state: connection to the server is not established");
		if (GetState() != State::InGame)
			throw StateException("Wrong state: player must be in the game");

		Send(COMMAND_END_GAME);
		state = State::Searching;
	}

	const std::string Connection::DEFAULT_URL = "aspidera-isitstoneserver.azurewebsites.net";
	const std::string Connection::DEFAULT_PORT = "80";
}
