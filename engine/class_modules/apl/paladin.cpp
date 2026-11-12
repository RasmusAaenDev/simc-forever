#include "class_modules/apl/paladin.hpp"

#include "player/action_priority_list.hpp"
#include "player/player.hpp"

namespace paladin_apl
{
std::string potion( const player_t* )
{
  return "disabled";
}

std::string flask( const player_t* )
{
  return "disabled";
}

std::string food( const player_t* )
{
  return "disabled";
}

std::string rune( const player_t* )
{
  return "disabled";
}

std::string temporary_enchant( const player_t* )
{
  return "disabled";
}
//paladin_apl_start
// AULA-RETTELSE: Retribution-basisrotation (Method/Warcraft Tavern): Seal of Command oppe (Seal of
// Righteousness hvis Seal of Command ikke er laert), Judgement paa nedkoeling, Hammer of Wrath under 20 %,
// Holy Strike paa nedkoeling, Consecration kun mod flere fjender og kun med over halv mana;
// auto-attack hele tiden (er manaen vaek, er auto-attack det eneste der sker).
void paladin( player_t* p )
{
  action_priority_list_t* default_ = p->get_action_priority_list( "default" );

  default_->add_action( "auto_attack" );
  default_->add_action( "seal_of_command,if=buff.seal_of_command.remains<1.5" );
  default_->add_action( "seal_of_righteousness,if=!buff.seal_of_command.up&buff.seal_of_righteousness.remains<1.5" );
  default_->add_action( "judgement" );
  default_->add_action( "hammer_of_wrath,if=target.health.pct<20" );
  default_->add_action( "holy_strike" );
  default_->add_action( "consecration,if=active_enemies>1&mana.pct>50" );
}
//paladin_apl_end
}  // namespace paladin_apl
