/*  Copyright 2014 MaidSafe.net limited

    This MaidSafe Software is licensed to you under (1) the MaidSafe.net Commercial License,
    version 1.0 or later, or (2) The General Public License (GPL), version 3, depending on which
    licence you accepted on initial access to the Software (the "Licences").

    By contributing code to the MaidSafe Software, or to this project generally, you agree to be
    bound by the terms of the MaidSafe Contributor Agreement, version 1.0, found in the root
    directory of this project at LICENSE, COPYING and CONTRIBUTOR respectively and also
    available at: http://www.maidsafe.net/licenses

    Unless required by applicable law or agreed to in writing, the MaidSafe Software distributed
    under the GPL Licence is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS
    OF ANY KIND, either express or implied.

    See the Licences for the specific language governing permissions and limitations relating to
    use of the MaidSafe Software.                                                                 */

#include "maidsafe/vault_manager/vault_config.h"

#include <utility>

namespace maidsafe {

namespace vault_manager {


VaultConfig::VaultConfig(const passport::Pmid& pmid_in,
                         const boost::filesystem::path& vault_dir_in,
                         const DiskUsage& max_disk_usage_in,
                         routing::BootstrapContacts bootstrap_contacts_in)
    : pmid(pmid_in),
      vault_dir(vault_dir_in),
      max_disk_usage(max_disk_usage_in),
#ifdef TESTING
      test_config(),
#endif
      bootstrap_contacts(bootstrap_contacts_in) {}

VaultConfig::VaultConfig(const VaultConfig& other)
    : pmid(other.pmid),
      vault_dir(other.vault_dir),
      max_disk_usage(other.max_disk_usage),
#ifdef TESTING
      test_config(other.test_config),
#endif
      bootstrap_contacts(other.bootstrap_contacts) {}

VaultConfig::VaultConfig(VaultConfig&& other)
    : pmid(std::move(other.pmid)),
      vault_dir(std::move(other.vault_dir)),
      max_disk_usage(std::move(other.max_disk_usage)),
#ifdef TESTING
      test_config(std::move(other.test_config)),
#endif
      bootstrap_contacts(std::move(other.bootstrap_contacts)) {}

VaultConfig& VaultConfig::operator=(VaultConfig other) {
  swap(*this, other);
  return *this;
}

void swap(VaultConfig& lhs, VaultConfig& rhs) {
  using std::swap;
  swap(lhs.pmid, rhs.pmid);
  swap(lhs.vault_dir, rhs.vault_dir);
  swap(lhs.max_disk_usage, rhs.max_disk_usage);
#ifdef TESTING
  swap(lhs.test_config, rhs.test_config);
#endif
  swap(lhs.bootstrap_contacts, rhs.bootstrap_contacts);
}


#ifdef TESTING

passport::Pmid GetPmidFromKeysFile(const boost::filesystem::path keys_path,
                                   size_t identity_index) {
  std::vector<passport::detail::AnmaidToPmid> key_chains(
      passport::detail::ReadKeyChainList(keys_path));
  if (identity_index >= key_chains.size()) {
    std::cout << "Identity selected out of bounds\n";
    BOOST_THROW_EXCEPTION(MakeError(CommonErrors::invalid_parameter));
  }
  return passport::Pmid(key_chains.at(identity_index).pmid);
}

std::vector<passport::PublicPmid> GetPublicPmidsFromKeysFile(
    const boost::filesystem::path keys_path) {
  std::vector<passport::detail::AnmaidToPmid> key_chains(
      passport::detail::ReadKeyChainList(keys_path));
  std::vector<passport::PublicPmid> public_pmids;
  for (auto& key_chain : key_chains)
    public_pmids.push_back(passport::PublicPmid(key_chain.pmid));
  return public_pmids;
}
#endif


}  // namespace vault_manager

}  // namespace maidsafe
