/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/
// SPDX-License-Identifier: GPL-3.0

#include <libyul/optimiser/CommonSwitchCasePrefixMover.h>

#include <libyul/optimiser/ASTCopier.h>
#include <libyul/optimiser/SyntacticalEquality.h>
#include <libyul/AST.h>
#include <libsolutil/CommonData.h>

#include <range/v3/algorithm/all_of.hpp>
#include <range/v3/algorithm/transform.hpp>
#include <range/v3/iterator.hpp>
#include <range/v3/view/drop.hpp>
#include <range/v3/view/transform.hpp>
#include <range/v3/view/zip.hpp>

using namespace std;
using namespace solidity;
using namespace solidity::yul;

namespace
{
class IdentifierReplacer: public ASTCopier
{
public:
	IdentifierReplacer(map<YulString, YulString> _identifierMap): m_identifierMap(move(_identifierMap)) {}
	using ASTCopier::operator();
protected:
	YulString translateIdentifier(YulString _name) override
	{
		if (YulString const* value = util::fetchValue(m_identifierMap, _name))
			return *value;
		else
			return _name;
	}
private:
	map<YulString, YulString> m_identifierMap;
};
}

void CommonSwitchCasePrefixMover::run(OptimiserStepContext&, Block& _ast)
{
	CommonSwitchCasePrefixMover{}(_ast);
}

void CommonSwitchCasePrefixMover::operator()(Block& _block)
{
	static constexpr auto eraseFirst = [](auto& _container) { _container.erase(begin(_container)); };
	util::iterateReplacing(
		_block.statements,
		[&](Statement& _s) -> optional<vector<Statement>>
		{
			visit(_s);
			if (Switch* switchStatement = get_if<Switch>(&_s))
			{
				yulAssert(!switchStatement->cases.empty(), "");
				vector<Statement> result;
				while (!switchStatement->cases.front().body.statements.empty())
				{
					Statement& referenceStatement = switchStatement->cases.front().body.statements.front();

					if (!ranges::all_of(
						switchStatement->cases | ranges::cpp20::views::drop(1),
						[&](Case& _case) {
						return !_case.body.statements.empty() &&
							SyntacticallyEqual{}(referenceStatement, _case.body.statements.front());
					}))
						break;

					if (VariableDeclaration const* referenceVarDecl = get_if<VariableDeclaration>(&referenceStatement))
					{
						for (Case& switchCase: switchStatement->cases | ranges::cpp20::views::drop(1))
						{
							VariableDeclaration* varDecl = std::get_if<VariableDeclaration>(&switchCase.body.statements.front());
							yulAssert(varDecl, "");
							yulAssert(varDecl->variables.size() == referenceVarDecl->variables.size(), "");

							static constexpr auto nameFromTypedName = [](TypedName const& _name) { return _name.name; };
							auto range = ranges::views::zip(
								varDecl->variables | ranges::views::transform(nameFromTypedName),
								referenceVarDecl->variables | ranges::views::transform(nameFromTypedName)
							);
							std::map<YulString, YulString> identifierReplacement{ranges::begin(range), ranges::end(range)};
							IdentifierReplacer replacer{{ranges::begin(range), ranges::end(range)}};
							vector<Statement> newBody;
							ranges::cpp20::transform(
								switchCase.body.statements | ranges::cpp20::views::drop(1),
								ranges::back_inserter(newBody),
								[&](auto const& _stmt) { return replacer.translate(_stmt); }
							);
							switchCase.body.statements = move(newBody);
						}
						result.emplace_back(move(referenceStatement));
						eraseFirst(switchStatement->cases.front().body.statements);
					}
					else
					{
						result.emplace_back(move(referenceStatement));
						for (Case& switchCase: switchStatement->cases)
							eraseFirst(switchCase.body.statements);
					}
				}
				if (!result.empty())
				{
					result.emplace_back(move(_s));
					return result;
				}
			}
			return {};
		}
	);
}
