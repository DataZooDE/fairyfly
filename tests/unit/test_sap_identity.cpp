// "SID/CLIENT/USER": the one spelling of an exact SAP identity (owner-mode endpoints, token grants, session
// workers), built in one place instead of ~15 hand-written concatenations.
#include <catch2/catch_test_macros.hpp>

#include "include/audit_log.h"
#include "include/auth/authorize.h"
#include "include/sap_identity.h"

using namespace fairyfly;

TEST_CASE("sap identity: SID/CLIENT/USER from facts, and from a session target", "[sap_identity]") {
    audit::SapFacts facts;
    facts.system = "BIG";
    facts.client = "100";
    facts.user = "JR";
    CHECK(sap_identity(facts) == "BIG/100/JR");
    CHECK(sap_identity_known(facts));
    facts.user.clear();
    CHECK_FALSE(sap_identity_known(facts));  // before logon: not an identity yet

    auth::SessionTarget target;
    target.system = "BIG/100";  // SessionTarget::system is SID/CLIENT
    target.user = "JR";
    CHECK(target.identity() == "BIG/100/JR");
}

TEST_CASE("sap identity: listed means listed exactly", "[sap_identity]") {
    const std::vector<std::string> granted = {"BIG/100/JR", "PRD/200/OPS"};
    CHECK(identity_listed(granted, "BIG/100/JR"));
    CHECK_FALSE(identity_listed(granted, "big/100/jr"));  // exact, the grants are stored upper-case
    CHECK_FALSE(identity_listed(granted, "BIG/100"));
    CHECK_FALSE(identity_listed({}, "BIG/100/JR"));
}
