BEGIN_SE()

ModManager* StaticSymbols::GetModManagerClient() const
{
    auto client = GetEoCClient();
    if (client == nullptr || client->ModManager == nullptr) {
        return nullptr;
    } else {
        return client->ModManager;
    }
}

ModManager* StaticSymbols::GetModManagerServer() const
{
    auto server = GetEoCServer();
    if (server == nullptr) {
        return nullptr;
    } else {
        return &server->ModManager;
    }
}

std::optional<ecl::GameState> StaticSymbols::GetClientState() const
{
    if (ecl__EoCClient != nullptr
        && *ecl__EoCClient != nullptr
        && (*ecl__EoCClient)->GameStateMachine != nullptr) {
        return (*ecl__EoCClient)->GameStateMachine->State;
    } else {
        return {};
    }
}

std::optional<esv::GameState> StaticSymbols::GetServerState() const
{
    if (esv__EoCServer != nullptr
        && *esv__EoCServer != nullptr
        && (*esv__EoCServer)->GameStateMachine != nullptr) {
        return (*esv__EoCServer)->GameStateMachine->State;
    } else {
        return {};
    }
}

esv::Level* StaticSymbols::GetCurrentServerLevel() const
{
    auto levelMgr = GetServerLevelManager();
    if (levelMgr) {
        return static_cast<esv::Level*>(levelMgr->CurrentLevel);
    } else {
        return nullptr;
    }
}

FixedString StaticSymbols::GetCurrentServerLevelName() const
{
    auto levelMgr = GetServerLevelManager();
    if (levelMgr && levelMgr->CurrentLevel) {
        return levelMgr->CurrentLevel->LevelData->LevelDesc->LevelName;
    } else {
        return {};
    }
}

ecl::Level* StaticSymbols::GetCurrentClientLevel() const
{
    auto levelMgr = GetClientLevelManager();
    if (levelMgr) {
        return static_cast<ecl::Level*>(levelMgr->CurrentLevel);
    } else {
        return nullptr;
    }
}

END_SE()
