#include "auth/system_catalog.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <sys/stat.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include "json/json.h"

namespace {

constexpr size_t SALT_BYTES = 16;
constexpr size_t HASH_BYTES = 32;

string hex_encode(const unsigned char *data, size_t size)
{
  static const char digits[] = "0123456789abcdef";
  string result(size * 2, '0');
  for (size_t i = 0; i < size; ++i) {
    result[i * 2] = digits[data[i] >> 4];
    result[i * 2 + 1] = digits[data[i] & 0x0f];
  }
  return result;
}

bool hex_decode(const string &input, vector<unsigned char> &output)
{
  if (input.size() % 2 != 0) return false;
  output.clear();
  output.reserve(input.size() / 2);
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < input.size(); i += 2) {
    int high = nibble(input[i]);
    int low = nibble(input[i + 1]);
    if (high < 0 || low < 0) return false;
    output.push_back(static_cast<unsigned char>((high << 4) | low));
  }
  return true;
}

bool contains(const vector<string> &items, const string &value)
{
  return std::find(items.begin(), items.end(), value) != items.end();
}

} // namespace

string SystemCatalog::uppercase(string value)
{
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::toupper(c); });
  return value;
}

bool SystemCatalog::valid_identifier(const string &value)
{
  if (value.empty() || value.size() > 64 || !(std::isalpha(static_cast<unsigned char>(value[0])) || value[0] == '_')) return false;
  return std::all_of(value.begin() + 1, value.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; });
}

RC SystemCatalog::set_password_unlocked(UserRecord &user, const string &password)
{
  if (password.size() < 8 || password.size() > 1024) return RC::INVALID_ARGUMENT;
  unsigned char salt[SALT_BYTES];
  unsigned char hash[HASH_BYTES];
  if (RAND_bytes(salt, sizeof(salt)) != 1) return RC::INTERNAL;
  if (PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()), salt, sizeof(salt),
          static_cast<int>(user.iterations), EVP_sha256(), sizeof(hash), hash) != 1) return RC::INTERNAL;
  user.salt_hex = hex_encode(salt, sizeof(salt));
  user.hash_hex = hex_encode(hash, sizeof(hash));
  return RC::SUCCESS;
}

RC SystemCatalog::initialize_new(const string &data_dir, const string &root_password, string &effective_root_password)
{
  if (root_password.empty()) return RC::INVALID_ARGUMENT;
  std::lock_guard<std::mutex> guard(mutex_);
  data_dir_ = data_dir;
  catalog_path_ = (std::filesystem::path(data_dir_) / "system" / "catalog.json").string();
  if (std::filesystem::exists(catalog_path_)) return RC::FILE_EXIST;
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(catalog_path_).parent_path(), ec);
  if (ec) return RC::IOERR_WRITE;
  effective_root_password = root_password;
  UserRecord root;
  root.id = next_user_id_++;
  root.username = "root";
  RC rc = set_password_unlocked(root, effective_root_password);
  if (OB_FAIL(rc)) return rc;
  users_.push_back(root);
  databases_.push_back("sys");
  return save_unlocked();
}

RC SystemCatalog::open_or_initialize(const string &data_dir, string &temporary_root_password, bool &created)
{
  data_dir_ = data_dir;
  catalog_path_ = (std::filesystem::path(data_dir_) / "system" / "catalog.json").string();
  created = !std::filesystem::exists(catalog_path_);
  if (created) return initialize_new(data_dir, "", temporary_root_password);
  std::lock_guard<std::mutex> guard(mutex_);
  return load_unlocked();
}

RC SystemCatalog::authenticate(const string &username, const string &password, UserInfo &user) const
{
  std::lock_guard<std::mutex> guard(mutex_);
  auto it = std::find_if(users_.begin(), users_.end(), [&](const UserRecord &record) { return record.username == username; });
  if (it == users_.end()) return RC::AUTHENTICATION_FAILED;
  vector<unsigned char> salt;
  vector<unsigned char> expected;
  if (!hex_decode(it->salt_hex, salt) || !hex_decode(it->hash_hex, expected) || expected.size() != HASH_BYTES) return RC::INTERNAL;
  unsigned char actual[HASH_BYTES];
  if (PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()), salt.data(), static_cast<int>(salt.size()),
          static_cast<int>(it->iterations), EVP_sha256(), sizeof(actual), actual) != 1) return RC::INTERNAL;
  if (CRYPTO_memcmp(expected.data(), actual, sizeof(actual)) != 0) return RC::AUTHENTICATION_FAILED;
  user = {it->id, it->username};
  return RC::SUCCESS;
}

RC SystemCatalog::create_user(const string &username, const string &password)
{
  std::lock_guard<std::mutex> guard(mutex_);
  if (!valid_identifier(username)) return RC::INVALID_ARGUMENT;
  if (std::any_of(users_.begin(), users_.end(), [&](const UserRecord &u) { return u.username == username; })) return RC::USER_EXIST;
  UserRecord user;
  user.id = next_user_id_++;
  user.username = username;
  RC rc = set_password_unlocked(user, password);
  if (OB_FAIL(rc)) return rc;
  users_.push_back(user);
  return save_unlocked();
}

RC SystemCatalog::alter_user(const string &username, const string &password)
{
  std::lock_guard<std::mutex> guard(mutex_);
  auto it = std::find_if(users_.begin(), users_.end(), [&](const UserRecord &u) { return u.username == username; });
  if (it == users_.end()) return RC::USER_NOT_EXIST;
  RC rc = set_password_unlocked(*it, password);
  return OB_FAIL(rc) ? rc : save_unlocked();
}

RC SystemCatalog::drop_user(const string &username)
{
  std::lock_guard<std::mutex> guard(mutex_);
  if (username == "root") return RC::AUTHORIZATION_DENIED;
  auto old_size = users_.size();
  users_.erase(std::remove_if(users_.begin(), users_.end(), [&](const UserRecord &u) { return u.username == username; }), users_.end());
  if (old_size == users_.size()) return RC::USER_NOT_EXIST;
  grants_.erase(std::remove_if(grants_.begin(), grants_.end(), [&](const PrivilegeGrant &g) { return g.username == username; }), grants_.end());
  return save_unlocked();
}

vector<UserInfo> SystemCatalog::users() const
{
  std::lock_guard<std::mutex> guard(mutex_);
  vector<UserInfo> result;
  for (const UserRecord &user : users_) result.push_back({user.id, user.username});
  return result;
}

RC SystemCatalog::add_database(const string &database)
{
  std::lock_guard<std::mutex> guard(mutex_);
  if (!valid_identifier(database)) return RC::INVALID_ARGUMENT;
  if (contains(databases_, database)) return RC::SCHEMA_DB_EXIST;
  databases_.push_back(database);
  return save_unlocked();
}

RC SystemCatalog::remove_database(const string &database)
{
  std::lock_guard<std::mutex> guard(mutex_);
  if (database == "sys") return RC::AUTHORIZATION_DENIED;
  auto it = std::find(databases_.begin(), databases_.end(), database);
  if (it == databases_.end()) return RC::SCHEMA_DB_NOT_EXIST;
  databases_.erase(it);
  grants_.erase(std::remove_if(grants_.begin(), grants_.end(), [&](const PrivilegeGrant &g) { return g.database == database; }), grants_.end());
  return save_unlocked();
}

bool SystemCatalog::database_exists(const string &database) const
{
  std::lock_guard<std::mutex> guard(mutex_);
  return contains(databases_, database);
}

vector<string> SystemCatalog::databases() const
{
  std::lock_guard<std::mutex> guard(mutex_);
  return databases_;
}

RC SystemCatalog::grant(const PrivilegeGrant &input)
{
  std::lock_guard<std::mutex> guard(mutex_);
  if (!std::any_of(users_.begin(), users_.end(), [&](const UserRecord &u) { return u.username == input.username; })) return RC::USER_NOT_EXIST;
  PrivilegeGrant grant = input;
  grant.scope = uppercase(grant.scope);
  for (string &privilege : grant.privileges) privilege = uppercase(privilege);
  auto it = std::find_if(grants_.begin(), grants_.end(), [&](const PrivilegeGrant &g) {
    return g.username == grant.username && g.scope == grant.scope && g.database == grant.database && g.table == grant.table;
  });
  if (it == grants_.end()) grants_.push_back(grant);
  else for (const string &privilege : grant.privileges) if (!contains(it->privileges, privilege)) it->privileges.push_back(privilege);
  return save_unlocked();
}

RC SystemCatalog::revoke(const PrivilegeGrant &input)
{
  std::lock_guard<std::mutex> guard(mutex_);
  for (PrivilegeGrant &grant : grants_) {
    if (grant.username == input.username && grant.scope == uppercase(input.scope) && grant.database == input.database && grant.table == input.table) {
      for (const string &privilege : input.privileges) {
        string upper = uppercase(privilege);
        grant.privileges.erase(std::remove(grant.privileges.begin(), grant.privileges.end(), upper), grant.privileges.end());
      }
    }
  }
  grants_.erase(std::remove_if(grants_.begin(), grants_.end(), [](const PrivilegeGrant &g) { return g.privileges.empty(); }), grants_.end());
  return save_unlocked();
}

vector<PrivilegeGrant> SystemCatalog::grants_for(const string &username) const
{
  std::lock_guard<std::mutex> guard(mutex_);
  vector<PrivilegeGrant> result;
  for (const PrivilegeGrant &grant : grants_) if (grant.username == username) result.push_back(grant);
  return result;
}

bool SystemCatalog::allowed(const string &username, const string &privilege, const string &database, const string &table) const
{
  if (username == "root") return true;
  std::lock_guard<std::mutex> guard(mutex_);
  string requested = uppercase(privilege);
  for (const PrivilegeGrant &grant : grants_) {
    if (grant.username != username) continue;
    if (requested == "CONNECT" && grant.database == database &&
        (grant.scope == "DATABASE" || grant.scope == "TABLE")) return true;
    bool resource_matches = grant.scope == "GLOBAL" ||
        (grant.scope == "DATABASE" && grant.database == database) ||
        (grant.scope == "TABLE" && grant.database == database && grant.table == table);
    if (resource_matches && (contains(grant.privileges, "ALL") || contains(grant.privileges, requested))) return true;
  }
  return false;
}

RC SystemCatalog::save_unlocked() const
{
  Json::Value root;
  root["format_version"] = 1;
  root["next_user_id"] = Json::UInt64(next_user_id_);
  for (const UserRecord &user : users_) {
    Json::Value value;
    value["id"] = Json::UInt64(user.id);
    value["username"] = user.username;
    value["password"]["algorithm"] = "PBKDF2-HMAC-SHA256";
    value["password"]["iterations"] = user.iterations;
    value["password"]["salt"] = user.salt_hex;
    value["password"]["hash"] = user.hash_hex;
    root["users"].append(value);
  }
  for (const string &database : databases_) root["databases"].append(database);
  for (const PrivilegeGrant &grant : grants_) {
    Json::Value value;
    value["username"] = grant.username;
    value["scope"] = grant.scope;
    value["database"] = grant.database;
    value["table"] = grant.table;
    for (const string &privilege : grant.privileges) value["privileges"].append(privilege);
    root["grants"].append(value);
  }
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "  ";
  string temp_path = catalog_path_ + ".tmp";
  std::ofstream out(temp_path, std::ios::trunc);
  if (!out) return RC::IOERR_WRITE;
  out << Json::writeString(builder, root) << '\n';
  out.close();
  if (!out) return RC::IOERR_WRITE;
  chmod(temp_path.c_str(), S_IRUSR | S_IWUSR);
  std::error_code ec;
  std::filesystem::rename(temp_path, catalog_path_, ec);
  return ec ? RC::IOERR_WRITE : RC::SUCCESS;
}

RC SystemCatalog::load_unlocked()
{
  std::ifstream in(catalog_path_);
  if (!in) return RC::IOERR_READ;
  Json::Value root;
  Json::CharReaderBuilder builder;
  string errors;
  if (!Json::parseFromStream(builder, in, &root, &errors)) return RC::JSON_PARSE_FAILED;
  next_user_id_ = root.get("next_user_id", 1).asUInt64();
  users_.clear(); databases_.clear(); grants_.clear();
  for (const Json::Value &value : root["users"]) {
    UserRecord user;
    user.id = value["id"].asUInt64();
    user.username = value["username"].asString();
    user.iterations = value["password"].get("iterations", 210000).asUInt();
    user.salt_hex = value["password"]["salt"].asString();
    user.hash_hex = value["password"]["hash"].asString();
    users_.push_back(user);
  }
  for (const Json::Value &value : root["databases"]) databases_.push_back(value.asString());
  for (const Json::Value &value : root["grants"]) {
    PrivilegeGrant grant;
    grant.username = value["username"].asString();
    grant.scope = value["scope"].asString();
    grant.database = value["database"].asString();
    grant.table = value["table"].asString();
    for (const Json::Value &privilege : value["privileges"]) grant.privileges.push_back(privilege.asString());
    grants_.push_back(grant);
  }
  return users_.empty() ? RC::JSON_MEMBER_MISSING : RC::SUCCESS;
}
