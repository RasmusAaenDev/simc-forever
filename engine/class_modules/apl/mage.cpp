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
// AULA-RETTELSE: Rotationen vaelges ud fra de valgte talenter.
// - Frost (Ice Lance, Fingers of Frost eller Ice Barrier): Frost-basisrotationen nedenfor.
// - Fire (Ignite, Pyroblast, Improved Scorch, Heating Up eller Combustion, men ingen
//   Frost-noegletalenter): listen "fire".
// - Ingen af delene: grundrotationen (Fire Blast paa nedkoeling, ellers Frostbolt; Fireball kun
//   foer Frostbolt er laert). Evocation naar manaen er lav; er der ikke mana til en spell, skydes
//   der med wand (Shoot). Talent-spells uden talentet er aldrig klar.
void mage( player_t* p )
{
  action_priority_list_t* default_ = p->get_action_priority_list( "default" );
  action_priority_list_t* fire     = p->get_action_priority_list( "fire" );

  const std::string frost_key = "(talent.ice_lance.enabled|talent.fingers_of_frost.enabled|talent.ice_barrier.enabled)";
  const std::string fire_key  = "(talent.ignite.enabled|talent.pyroblast.enabled|talent.improved_scorch.enabled|"
                                "talent.heating_up.enabled|talent.combustion.enabled)";

  default_->add_action( "evocation,if=mana.pct<10" );
  default_->add_action( "call_action_list,name=fire,if=" + fire_key + "&!" + frost_key );
  default_->add_action( "ice_barrier,if=!buff.ice_barrier.up" );
  default_->add_action( "ice_lance,if=buff.fingers_of_frost.up|debuff.frozen.up" );
  default_->add_action( "fire_blast" );
  default_->add_action( "frostbolt" );
  default_->add_action( "fireball" );
  default_->add_action( "shoot" );

  // Fire: Pyroblast som aabner, Scorch til 5 stak Fire Vulnerability (og igen foer udloeb),
  // Combustion paa nedkoeling, Pyroblast ved 3 Heating Up, Fire Blast paa nedkoeling, Fireball.
  fire->add_action( "pyroblast,if=time=0" );
  fire->add_action( "scorch,if=talent.improved_scorch.enabled&(debuff.fire_vulnerability.stack<5|debuff.fire_vulnerability.remains<5)" );
  fire->add_action( "combustion" );
  fire->add_action( "pyroblast,if=buff.heating_up.stack>=3" );
  fire->add_action( "fire_blast" );
  fire->add_action( "fireball" );
  fire->add_action( "shoot" );
}
//mage_apl_end
}  // namespace mage_apl
