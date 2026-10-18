// Asks the person at the keyboard to confirm a state-changing AI action.
#pragma once
#include <memory>
#include <string>

namespace ffy::link {

class Approver {
public:
    virtual ~Approver() = default;
    virtual bool approve(const std::string& text) = 0;
};

std::shared_ptr<Approver> fixed_approver(bool answer);  // tests, or an explicitly unattended machine (--approve yes)
// Windows: topmost Yes/No dialog on the interactive desktop (no answer in 60 s = deny).
// Other platforms: denies (there is no desktop dialog); use fixed_approver(true) for unattended test rigs.
std::shared_ptr<Approver> desktop_approver();

}  // namespace ffy::link
