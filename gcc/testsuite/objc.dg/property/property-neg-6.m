/* Check for proper declaration of @property. */
/* { dg-do compile } */

@interface Bar
{
  int iVar;
}
@property int FooBar /* { dg-error "expected ':', ',', ';', '\}' or '__attribute__' at end of input" } */
