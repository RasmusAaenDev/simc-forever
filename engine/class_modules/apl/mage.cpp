#include "class_modules/apl/mage.hpp"

#include "player/action_priority_list.hpp"
#include "player/player.hpp"

namespace mage_apl
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
//mage_apl_start
// AULA-RETTELSE: Frost-basisrotation. Evocation naar manaen er lav, Fire Blast paa
// nedkoeling, ellers Frostbolt; Fireball kun foer Frostbolt er laert (level 1-3).
// Er der ikke mana til en spell, skydes der med wand (Shoot) mens manaen kommer igen.
void mage( player_t* p )
{
  action_priority_list_t* default_ = p->get_action_priority_list( "default" );

  default_->add_action( "evocation,if=mana.pct<10" );
  default_->add_action( "fire_blast" );
  default_->add_action( "frostbolt" );
  default_->add_action( "fireball" );
  default_->add_action( "shoot" );
}
//mage_apl_end
}  // namespace mage_apl
