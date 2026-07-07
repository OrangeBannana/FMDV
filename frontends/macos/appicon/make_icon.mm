// Generates FMDV's 1024x1024 app-icon master PNG with CoreGraphics/CoreText:
// the Markdown "M + down arrow" mark in graphite ink over FMDV's link-blue
// heading rule, on a paper-white squircle. `make app` rasterizes it into the
// iconset. Usage: ./make_icon icon-1024.png
#import <CoreGraphics/CoreGraphics.h>
#import <CoreText/CoreText.h>
#import <ImageIO/ImageIO.h>
#include <cmath>

static CGColorRef RGB(CGColorSpaceRef cs, int r,int g,int b,double a=1){ CGFloat c[4]={r/255.0,g/255.0,b/255.0,(CGFloat)a}; return CGColorCreate(cs,c);}

int main(int argc,char**argv){
  const char* out = argc>1?argv[1]:"icon.png";
  const double S=1024;
  CGColorSpaceRef cs=CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGContextRef ctx=CGBitmapContextCreate(NULL,(size_t)S,(size_t)S,8,0,cs,kCGImageAlphaPremultipliedLast);

  // squircle tile
  double m=96, sz=S-2*m, r=200;
  CGRect tile=CGRectMake(m,m,sz,sz);
  CGPathRef path=CGPathCreateWithRoundedRect(tile,r,r,NULL);

  // gradient fill white(top)->light gray(bottom)
  CGContextSaveGState(ctx);
  CGContextAddPath(ctx,path); CGContextClip(ctx);
  CGColorRef c1=RGB(cs,255,255,255), c2=RGB(cs,235,238,243);
  const void* carr[2]={c1,c2}; CFArrayRef colors=CFArrayCreate(NULL,carr,2,&kCFTypeArrayCallBacks);
  CGFloat locs[2]={0,1};
  CGGradientRef grad=CGGradientCreateWithColors(cs,colors,locs);
  CGContextDrawLinearGradient(ctx,grad,CGPointMake(0,S),CGPointMake(0,0),0);
  CGContextRestoreGState(ctx);

  // subtle edge
  CGContextAddPath(ctx,path); CGContextSetStrokeColorWithColor(ctx,RGB(cs,224,228,233)); CGContextSetLineWidth(ctx,3); CGContextStrokePath(ctx);

  CGColorRef ink=RGB(cs,0x24,0x29,0x2f), blue=RGB(cs,0x09,0x69,0xda);

  // "M" bold system font
  double fpx=440;
  CTFontRef base=CTFontCreateUIFontForLanguage(kCTFontUIFontSystem,fpx,NULL);
  CTFontRef mf=CTFontCreateCopyWithSymbolicTraits(base,fpx,NULL,kCTFontTraitBold,kCTFontTraitBold); if(!mf)mf=base;
  CFStringRef ks[2]={kCTFontAttributeName,kCTForegroundColorAttributeName}; CFTypeRef vs[2]={mf,ink};
  CFDictionaryRef at=CFDictionaryCreate(NULL,(const void**)ks,(const void**)vs,2,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
  CFAttributedStringRef as=CFAttributedStringCreate(NULL,CFSTR("M"),at);
  CTLineRef line=CTLineCreateWithAttributedString(as);
  double mw=CTLineGetTypographicBounds(line,NULL,NULL,NULL);
  double capH=CTFontGetCapHeight(mf);

  double arrowW=180, gap=34;
  double groupW=mw+gap+arrowW;
  double gx=(S-groupW)/2;
  double cy=548;                         // vertical center of the mark (CG: up=+y)
  CGContextSetTextPosition(ctx,gx,cy-capH/2);
  CTLineDraw(line,ctx);

  // down arrow (bar + solid head), same ink, aligned to cap height
  double axc=gx+mw+gap+arrowW/2, barW=58;
  double headH=132, headTop=cy-capH/2+headH, apex=cy-capH/2, barTop=cy+capH/2;
  CGContextSetFillColorWithColor(ctx,ink);
  CGContextFillRect(ctx,CGRectMake(axc-barW/2,headTop-6,barW,barTop-(headTop-6)));
  CGContextBeginPath(ctx);
  CGContextMoveToPoint(ctx,axc,apex);
  CGContextAddLineToPoint(ctx,axc-arrowW/2,headTop);
  CGContextAddLineToPoint(ctx,axc+arrowW/2,headTop);
  CGContextClosePath(ctx); CGContextFillPath(ctx);

  // accent rule beneath the mark (FMDV's heading underline)
  double rw=372, rh=30, ry=316;
  CGPathRef rule=CGPathCreateWithRoundedRect(CGRectMake((S-rw)/2,ry,rw,rh),rh/2,rh/2,NULL);
  CGContextAddPath(ctx,rule); CGContextSetFillColorWithColor(ctx,blue); CGContextFillPath(ctx);

  CGImageRef img=CGBitmapContextCreateImage(ctx);
  CFStringRef p=CFStringCreateWithCString(NULL,out,kCFStringEncodingUTF8);
  CFURLRef url=CFURLCreateWithFileSystemPath(NULL,p,kCFURLPOSIXPathStyle,false);
  CGImageDestinationRef d=CGImageDestinationCreateWithURL(url,CFSTR("public.png"),1,NULL);
  CGImageDestinationAddImage(d,img,NULL); CGImageDestinationFinalize(d);
  return 0;
}
