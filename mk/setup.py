from xcp.supplementalpack import *
from optparse import OptionParser

parser = OptionParser()
parser.add_option('--pdn', dest="product_name")
parser.add_option('--pdv', dest="product_version")
parser.add_option('--bld', dest="build")
parser.add_option('--out', dest="outdir")
parser.add_option('--spn', dest="sp_name")
parser.add_option('--spd', dest="sp_description")
parser.add_option('--rxv', dest="req_xs_version")
(options, args) = parser.parse_args()

xs = Requires(originator='xs', name='main', test='ge',
               product=options.product_name, version=options.req_xs_version)

setup(originator='xs', name=options.sp_name, product=options.product_name,
      version=options.product_version, build=options.build, vendor='Citrix Systems, Inc.',
      description=options.sp_description, packages=args, requires=[xs],
      outdir=options.outdir, output=['iso'])
