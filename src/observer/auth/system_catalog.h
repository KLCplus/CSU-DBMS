#pragma once

#include <cstdint>
#include <mutex>

#include "common/lang/string.h"
#include "common/lang/vector.h"
#include "common/sys/rc.h"

struct UserInfo
{
  uint64_t id = 0;
  string username;
};

struct PrivilegeGrant
{
  string username;
  string scope; // GLOBAL, DATABASE, TABLE
  string database;
  string table;
  vector<string> privileges;
};

/** Persistent product metadata. Passwords use PBKDF2-HMAC-SHA256 with a random salt. */
class SystemCatalog
{
public:
  RC open_or_initialize(const string &data_dir, string &temporary_root_password, bool &created);
  RC initialize_new(const string &data_dir, const string &root_password, string &effective_root_password);

  RC authenticate(const string &username, const string &password, UserInfo &user) const;
  RC create_user(const string &username, const string &password);
  RC alter_user(const string &username, const string &password);
  RC drop_user(const string &username);
  vector<UserInfo> users() const;

  RC add_database(const string &database);
  RC remove_database(const string &database);
  bool database_exists(const string &database) const;
  vector<string> databases() const;

  RC grant(const PrivilegeGrant &grant);
  RC revoke(const PrivilegeGrant &grant);
  vector<PrivilegeGrant> grants_for(const string &username) const;
  bool allowed(const string &username, const string &privilege, const string &database, const string &table = "") const;

  const string &catalog_path() const { return catalog_path_; }

private:
  struct UserRecord
  {
    uint64_t id = 0;
    string username;
    string salt_hex;
    string hash_hex;
    uint32_t iterations = 210000;
  };

  RC load_unlocked();
  RC save_unlocked() const;
  RC set_password_unlocked(UserRecord &user, const string &password);
  static bool valid_identifier(const string &value);
  static string uppercase(string value);

  mutable std::mutex mutex_;
  string data_dir_;
  string catalog_path_;
  uint64_t next_user_id_ = 1;
  vector<UserRecord> users_;
  vector<string> databases_;
  vector<PrivilegeGrant> grants_;
};
