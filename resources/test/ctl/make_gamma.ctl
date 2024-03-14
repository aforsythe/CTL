const float gamutCuspTable[360][3] = 
{
    { 54.1476177130,  67.8669042191,  0.8549357905 },
    { 53.8600307220,  68.3057302802,  1.8725432481 },
    { 53.5697051711,  68.7621370936,  2.9237082164 },
    { 53.2764994869,  69.2371810347,  4.0125972684 },
    { 52.9802514755,  69.7321161051,  5.1442429981 },
    { 52.6807728454,  70.2484603321,  6.3247976021 },
    { 52.3778415870,  70.7880923332,  7.5618880541 },
    { 52.0711910756,  71.3533956493,  8.8651269197 },
    { 51.7604939724,  71.9474815102,  10.2468702949 },
    { 51.4453375104,  72.5745460195,  11.7233851887 },
    { 51.1251837396,  73.2404700278,  13.3167312478 },
    { 50.7993017606,  73.9538864090,  15.0579712101 },
    { 50.4666433072,  74.7282249980,  16.9930636149 },
    { 50.1255904928,  75.5860398987,  19.1947919213 },
    { 49.7733671282,  76.5695572697,  21.7905246576 },
    { 49.4043270882,  77.7727734904,  25.0425742348 },
    { 50.4892243396,  74.6422746734,  26.5158751188 },
    { 51.5423867377,  71.8021770347,  28.0113732628 },
    { 52.5667218028,  69.2174412657,  29.5284860512 },
    { 53.5646837277,  66.8593432142,  31.0663367804 },
    { 54.5383702365,  64.7040213604,  32.6237820966 },
    { 55.4895939304,  62.7314165695,  34.1994349870 },
    { 56.4199359376,  60.9244828187,  35.7916856950 },
    { 57.3307869919,  59.2685895201,  37.3987221212 },
    { 58.2233793971,  57.7510621461,  39.0185507181 },
    { 59.0988122555,  56.3608245577,  40.6490184807 },
    { 59.9580716373,  55.0881173872,  42.2878363244 },
    { 60.8020468930,  53.9242741661,  43.9326038983 },
    { 61.6315439853,  52.8615419098,  45.5808356839 },
    { 62.4472964876,  51.8929363542,  47.2299880753 },
    { 63.2499747392,  51.0121245080,  48.8774870192 },
    { 64.0401935239,  50.2133289469,  50.5207557106 },
    { 64.8185185601,  49.4912495571,  52.1572417973 },
    { 65.5854720215,  48.8409993774,  53.7844435343 },
    { 66.3415372637,  48.2580518926,  55.3999343527 },
    { 67.0871628937,  47.7381976575,  57.0013853564 },
    { 67.8227662927,  47.2775085399,  58.5865853360 },
    { 68.5487366809,  46.8723081837,  60.1534579793 },
    { 69.2654377965,  46.5191475455,  61.7000760581 },
    { 69.9732102468,  46.2147845552,  63.2246724788 },
    { 70.6723735820,  45.9561671179,  64.7256481843 },
    { 71.3632281285,  45.7404188063,  66.2015769907 },
    { 72.0460566181,  45.5648267087,  67.6512075246 },
    { 72.7211256395,  45.4268309912,  69.0734624923 },
    { 73.3886869357,  45.3240158141,  70.4674355655 },
    { 74.0489785671,  45.2541013105,  71.8323861991 },
    { 74.7022259580,  45.2149363948,  73.1677327150 },
    { 75.3486428393,  45.2044922149,  74.4730439909 },
    { 75.9884321018,  45.2208561049,  75.7480300801 },
    { 76.6217865682,  45.2622259287,  76.9925320739 },
    { 77.2488896954,  45.3269047313,  78.2065114862 },
    { 77.8699162130,  45.4132956371,  79.3900394164 },
    { 78.4850327067,  45.5198969543,  80.5432857044 },
    { 79.0943981504,  45.6452974556,  81.6665082643 },
    { 79.6981643948,  45.7881718155,  82.7600427443 },
    { 80.2964766144,  45.9472761963,  83.8242926320 },
    { 80.8894737199,  46.1214439749,  84.8597198914 },
    { 81.4772887361,  46.3095816096,  85.8668361948 },
    { 82.0600491524,  46.5106646484,  86.8461947890 },
    { 82.6378772448,  46.7237338792,  87.7983830129 },
    { 83.2108903747,  46.9478916256,  88.7240154730 },
    { 83.7792012657,  47.1822981895,  89.6237278653 },
    { 84.3429182601,  47.4261684429,  90.4981714246 },
    { 84.9021455569,  47.6787685702,  91.3480079735 },
    { 85.4569834341,  47.9394129602,  92.1739055398 },
    { 86.0075284547,  48.2074612481,  92.9765345029 },
    { 86.5538736594,  48.4823155060,  93.7565642347 },
    { 87.0961087463,  48.7634175791,  94.5146601919 },
    { 87.6343202385,  49.0502465645,  95.2514814230 },
    { 88.1685916416,  49.3423164297,  95.9676784513 },
    { 88.6990035906,  49.6391737661,  96.6638914954 },
    { 89.2256339878,  49.9403956723,  97.3407489945 },
    { 89.7485581320,  50.2455877638,  97.9988664043 },
    { 90.2678488405,  50.5543823022,  98.6388452329 },
    { 90.7835765630,  50.8664364395,  99.2612722890 },
    { 91.2958094889,  51.1814305721,  99.8667191154 },
    { 91.1011887692,  51.1583743413,  100.4642996151 },
    { 90.9060290102,  51.1410066218,  101.0653711856 },
    { 90.7103258094,  51.1294200420,  101.6698325502 },
    { 90.5140747029,  51.1237070224,  102.2775783162 },
    { 90.3172711639,  51.1239597089,  102.8884990352 },
    { 90.1199106021,  51.1302699062,  103.5024812735 },
    { 89.9219883617,  51.1427290111,  104.1194076954 },
    { 89.7234997204,  51.1614279465,  104.7391571569 },
    { 89.5244398883,  51.1864570954,  105.3616048114 },
    { 89.3248040060,  51.2179062360,  105.9866222270 },
    { 89.1245871436,  51.2558644780,  106.6140775149 },
    { 88.9237842990,  51.3004201996,  107.2438354689 },
    { 88.7223903966,  51.3516609871,  107.8757577167 },
    { 88.5204002854,  51.4096735746,  108.5097028802 },
    { 88.3178087378,  51.4745437875,  109.1455267476 },
    { 88.1146104479,  51.5463564873,  109.7830824544 },
    { 87.9108000297,  51.6251955192,  110.4222206744 },
    { 87.7063720152,  51.7111436620,  111.0627898184 },
    { 87.5013208533,  51.8042825820,  111.7046362422 },
    { 87.2956409071,  51.9046927891,  112.3476044616 },
    { 87.0893264530,  52.0124535972,  112.9915373735 },
    { 86.8823716777,  52.1276430872,  113.6362764847 },
    { 86.6747706773,  52.2503380754,  114.2816621437 },
    { 86.4665174545,  52.3806140839,  114.9275337788 },
    { 86.2576059167,  52.5185453175,  115.5737301386 },
    { 86.0480298742,  52.6642046428,  116.2200895353 },
    { 85.8377830375,  52.8176635732,  116.8664500899 },
    { 85.6268590153,  52.9789922578,  117.5126499779 },
    { 85.4152513121,  53.1482594755,  118.1585276751 },
    { 85.2029533257,  53.3255326335,  118.8039222022 },
    { 84.9899583449,  53.5108777705,  119.4486733672 },
    { 84.7762595466,  53.7043595654,  120.0926220053 },
    { 84.5618499937,  53.9060413505,  120.7356102144 },
    { 84.3467226317,  54.1159851294,  121.3774815868 },
    { 84.1308702864,  54.3342516006,  122.0180814343 },
    { 83.9142856610,  54.5609001850,  122.6572570083 },
    { 83.6969613326,  54.7959890592,  123.2948577118 },
    { 83.4788897498,  55.0395751927,  123.9307353044 },
    { 83.2600632289,  55.2917143909,  124.5647440983 },
    { 83.0404739512,  55.5524613413,  125.1967411461 },
    { 82.8201139591,  55.8218696656,  125.8265864187 },
    { 82.5989751530,  56.0999919757,  126.4541429730 },
    { 82.3770492873,  56.3868799334,  127.0792771097 },
    { 82.1543279670,  56.6825843152,  127.7018585202 },
    { 81.9308026439,  56.9871550808,  128.3217604224 },
    { 81.7064646120,  57.3006414449,  128.9388596854 },
    { 81.4813050043,  57.6230919538,  129.5530369427 },
    { 81.2553147878,  57.9545545645,  130.1641766940 },
    { 81.0284847593,  58.2950767277,  130.7721673953 },
    { 80.8008055412,  58.6447054743,  131.3769015379 },
    { 80.5722675761,  59.0034875044,  131.9782757155 },
    { 80.3428611226,  59.3714692797,  132.5761906805 },
    { 80.1125762498,  59.7486971184,  133.1705513882 },
    { 79.8814028323,  60.1352172928,  133.7612670317 },
    { 79.6493305448,  60.5310761295,  134.3482510647 },
    { 79.4163488560,  60.9363201118,  134.9314212148 },
    { 79.1824470236,  61.3509959845,  135.5106994864 },
    { 78.9476140876,  61.7751508605,  136.0860121546 },
    { 78.7118388643,  62.2088323299,  136.6572897492 },
    { 78.4751099398,  62.6520885704,  137.2244670305 },
    { 78.6687269551,  60.9596074439,  138.1191057892 },
    { 78.8602045344,  59.3865851766,  139.0132919010 },
    { 79.0497764501,  57.9180438985,  139.9088107920 },
    { 79.2376305322,  56.5419922726,  140.8070581309 },
    { 79.4239204170,  55.2486614317,  141.7091350754 },
    { 79.6087737053,  54.0299741595,  142.6159139292 },
    { 79.7922977843,  52.8791659130,  143.5280845959 },
    { 79.9745840806,  51.7905078223,  144.4461881854 },
    { 80.1557112306,  50.7591000476,  145.3706418033 },
    { 80.3357474862,  49.7807148307,  146.3017571544 },
    { 80.5147525709,  48.8516753766,  147.2397547301 },
    { 80.6927791293,  47.9687610490,  148.1847747934 },
    { 80.8698738763,  47.1291322072,  149.1368860165 },
    { 81.0460785168,  46.3302699255,  150.0960923824 },
    { 81.2214304899,  45.5699271366,  151.0623387970 },
    { 81.3959635774,  44.8460886558,  152.0355157426 },
    { 81.5697084045,  44.1569381842,  153.0154632211 },
    { 81.7426928556,  43.5008308540,  154.0019741764 },
    { 81.9149424227,  42.8762702178,  154.9947975413 },
    { 82.0864804986,  42.2818888333,  155.9936410215 },
    { 82.2573286245,  41.7164317806,  156.9981737036 },
    { 82.4275067027,  41.1787425925,  158.0080285560 },
    { 82.5970331771,  40.6677511834,  159.0228048731 },
    { 82.7659251903,  40.1824634476,  160.0420707040 },
    { 82.9341987183,  39.7219522597,  161.0653652936 },
    { 83.1018686883,  39.2853496623,  162.0922015568 },
    { 83.2689490812,  38.8718400645,  163.1220685983 },
    { 83.4354530217,  38.4806543053,  164.1544342850 },
    { 83.6013928575,  38.1110644650,  165.1887478710 },
    { 83.7667802290,  37.7623793212,  166.2244426719 },
    { 83.9316261313,  37.4339403688,  167.2609387802 },
    { 84.0959409691,  37.1251183323,  168.2976458109 },
    { 84.2597346059,  36.8353101098,  169.3339656629 },
    { 84.4230164076,  36.5639360997,  170.3692952793 },
    { 84.5857952818,  36.3104378628,  171.4030293902 },
    { 84.7480797135,  36.0742760851,  172.4345632178 },
    { 84.9098777963,  35.8549288039,  173.4632951246 },
    { 85.0711972614,  35.6518898712,  174.4886291857 },
    { 85.2320455038,  35.4646676257,  175.5099776658 },
    { 85.3924296054,  35.2927837515,  176.5267633829 },
    { 85.5523563570,  35.1357723018,  177.5384219424 },
    { 85.7118322772,  34.9931788686,  178.5444038262 },
    { 85.8708636308,  34.8645598824,  179.5441763229 },
    { 86.0294564447,  34.7494820242,  180.5372252895 },
    { 86.1876165231,  34.6475217381,  181.5230567351 },
    { 86.3453494610,  34.5582648294,  182.5011982197 },
    { 86.5026606568,  34.4813061384,  183.4712000653 },
    { 86.6595553239,  34.4162492769,  184.4326363764 },
    { 86.8160385015,  34.3627064199,  185.3851058715 },
    { 86.9721150645,  34.3202981425,  186.3282325284 },
    { 87.1277897322,  34.2886532941,  187.2616660469 },
    { 87.2830670773,  34.2674089026,  188.1850821375 },
    { 87.4379515333,  34.2562101034,  189.0981826425 },
    { 87.5924474023,  34.2547100852,  190.0006954997 },
    { 87.7465588609,  34.2625700508,  190.8923745596 },
    { 87.9002899675,  34.2794591851,  191.7729992666 },
    { 88.0536446675,  34.3050546297,  192.6423742179 },
    { 88.2066267990,  34.3390414581,  193.5003286101 },
    { 88.3592400977,  34.3811126513,  194.3467155886 },
    { 88.5114882021,  34.4309690700,  195.1814115109 },
    { 87.9776410584,  34.2609262425,  196.0184345088 },
    { 87.4398814951,  34.0974188512,  196.8714638643 },
    { 86.8981283012,  33.9407587683,  197.7405841493 },
    { 86.3522974404,  33.7912681435,  198.6258537321 },
    { 85.8023019121,  33.6492794156,  199.5273024822 },
    { 85.2480516035,  33.5151352882,  200.4449294381 },
    { 84.6894531320,  33.3891886677,  201.3787004575 },
    { 84.1264096771,  33.2718025614,  202.3285458708 },
    { 83.5588208010,  33.1633499356,  203.2943581632 },
    { 82.9865822577,  33.0642135296,  204.2759897123 },
    { 82.4095857878,  32.9747856290,  205.2732506119 },
    { 81.8277188999,  32.8954677954,  206.2859066139 },
    { 81.2408646356,  32.8266705551,  207.3136772217 },
    { 80.6489013188,  32.7688130482,  208.3562339734 },
    { 80.0517022849,  32.7223226416,  209.4131989487 },
    { 79.4491355911,  32.6876345098,  210.4841435391 },
    { 78.8410637043,  32.6651911880,  211.5685875159 },
    { 78.2273431646,  32.6554421053,  212.6659984331 },
    { 77.6078242228,  32.6588431063,  213.7757913958 },
    { 76.9823504487,  32.6758559681,  214.8973292246 },
    { 76.3507583074,  32.7069479265,  216.0299230404 },
    { 75.7128767009,  32.7525912220,  217.1728332881 },
    { 75.0685264706,  32.8132626792,  218.3252712112 },
    { 74.4175198570,  32.8894433354,  219.4864007834 },
    { 73.7596599126,  32.9816181344,  220.6553410911 },
    { 73.0947398610,  33.0902757029,  221.8311691560 },
    { 72.4225423994,  33.2159082280,  223.0129231742 },
    { 71.7428389342,  33.3590114564,  224.1996061399 },
    { 71.0553887451,  33.5200848347,  225.3901898117 },
    { 70.3599380679,  33.6996318133,  226.5836189710 },
    { 69.6562190859,  33.8981603357,  227.7788159118 },
    { 68.9439488189,  34.1161835366,  228.9746850945 },
    { 68.2228278972,  34.3542206729,  230.1701178890 },
    { 67.4925392050,  34.6127983135,  231.3639973255 },
    { 66.7527463763,  34.8924518126,  232.5552027696 },
    { 66.0030921229,  35.1937270980,  233.7426144314 },
    { 65.2431963712,  35.5171828030,  234.9251176185 },
    { 64.4726541805,  35.8633927761,  236.1016066419 },
    { 63.6910334105,  36.2329490083,  237.2709882792 },
    { 62.8978720997,  36.6264650200,  238.4321847051 },
    { 62.0926755108,  37.0445797592,  239.5841357918 },
    { 61.2749127883,  37.4879620703,  240.7258006849 },
    { 60.4440131663,  37.9573158054,  241.8561585533 },
    { 59.5993616478,  38.4533856645,  242.9742084056 },
    { 58.7402940648,  38.9769638693,  244.0789678542 },
    { 57.8660914064,  39.5288978011,  245.1694706904 },
    { 56.9759732766,  40.1100987627,  246.2447631074 },
    { 56.0690903153,  40.7215520635,  247.3038983716 },
    { 55.1445153727,  41.3643286807,  248.3459296910 },
    { 54.2012331762,  42.0395988104,  249.3699009540 },
    { 53.2381281648,  42.7486477150,  250.3748349147 },
    { 52.2539700761,  43.4928943821,  251.3597182576 },
    { 51.2473967553,  44.2739136649,  252.3234827835 },
    { 50.2168935049,  45.0934627802,  253.2649816821 },
    { 49.1607680811,  45.9535133195,  254.1829594778 },
    { 48.0771201604,  46.8562903240,  255.0760136796 },
    { 46.9638037017,  47.8043205241,  255.9425453757 },
    { 45.8183800714,  48.8004926434,  256.7806948350 },
    { 44.6380589943,  49.8481338356,  257.5882564238 },
    { 43.4196232234,  50.9511080775,  258.3625644569 },
    { 43.9071757392,  50.1913042715,  260.1864564940 },
    { 44.3873271978,  49.4974096345,  262.0116262379 },
    { 44.8604103319,  48.8666194296,  263.8356450367 },
    { 45.3267332595,  48.2962471694,  265.6560374573 },
    { 45.7865819711,  47.7837077085,  267.4703090931 },
    { 46.2402225030,  47.3265037478,  269.2759742571 },
    { 46.6879028429,  46.9222152997,  271.0705829517 },
    { 47.1298546085,  46.5684917060,  272.8517465314 },
    { 47.5662945295,  46.2630458327,  274.6171615190 },
    { 47.9974257620,  46.0036500930,  276.3646311040 },
    { 48.4234390565,  45.7881339782,  278.0920839473 },
    { 48.8445138002,  45.6143827986,  279.7975900142 },
    { 49.2608189490,  45.4803373645,  281.4793732689 },
    { 49.6725138629,  45.3839943617,  283.1358211699 },
    { 50.0797490579,  45.3234072058,  284.7654910090 },
    { 50.4826668830,  45.2966871861,  286.3671132260 },
    { 50.8814021321,  45.3020047398,  287.9395919068 },
    { 51.2760825982,  45.3375907239,  289.4820027335 },
    { 51.6668295757,  45.4017375791,  290.9935886946 },
    { 52.0537583182,  45.4928003062,  292.4737538898 },
    { 52.4369784545,  45.6091971967,  293.9220557730 },
    { 52.8165943687,  45.7494102801,  295.3381961718 },
    { 53.1927055483,  45.9119854679,  296.7220114068 },
    { 53.5654069023,  46.0955323892,  298.0734618113 },
    { 53.9347890537,  46.2987239238,  299.3926209186 },
    { 54.3009386079,  46.5202954475,  300.6796645554 },
    { 54.6639383999,  46.7590438137,  301.9348600400 },
    { 55.0238677227,  47.0138260956,  303.1585556522 },
    { 55.3808025373,  47.2835581209,  304.3511705066 },
    { 55.7348156676,  47.5672128278,  305.5131849307 },
    { 56.0859769802,  47.8638184753,  306.6451314209 },
    { 56.4343535521,  48.1724567362,  307.7475862249 },
    { 56.7800098249,  48.4922607017,  308.8211615772 },
    { 57.1230077494,  48.8224128247,  309.8664985982 },
    { 57.4634069197,  49.1621428249,  310.8842608513 },
    { 57.8012646978,  49.5107255774,  311.8751285417 },
    { 58.1366363302,  49.8674790033,  312.8397933328 },
    { 58.4695750565,  50.2317619796,  313.7789537481 },
    { 58.8001322113,  50.6029722804,  314.6933111225 },
    { 59.1283573190,  50.9805445622,  315.5835660663 },
    { 59.4542981828,  51.3639484021,  316.4504154006 },
    { 59.7780009684,  51.7526863963,  317.2945495244 },
    { 60.0995102820,  52.1462923244,  318.1166501757 },
    { 60.4188692438,  52.5443293838,  318.9173885466 },
    { 60.7361195573,  52.9463884972,  319.6974237174 },
    { 61.0513015742,  53.3520866941,  320.4574013753 },
    { 61.3644543556,  53.7610655677,  321.1979527863 },
    { 61.6756157295,  54.1729898070,  321.9196939894 },
    { 61.9848223452,  54.5875458030,  322.6232251880 },
    { 62.2921097249,  55.0044403279,  323.3091303120 },
    { 62.5975123118,  55.4233992849,  323.9779767293 },
    { 62.9010635159,  55.8441665280,  324.6303150858 },
    { 63.2027957576,  56.2665027477,  325.2666792563 },
    { 63.5027405085,  56.6901844209,  325.8875863891 },
    { 63.8009283301,  57.1150028229,  326.4935370315 },
    { 64.0973889107,  57.5407630966,  327.0850153215 },
    { 64.3921511004,  57.9672833796,  327.6624892353 },
    { 64.6852429440,  58.3943939824,  328.2264108811 },
    { 64.9766917125,  58.8219366187,  328.7772168294 },
    { 65.2665239332,  59.2497636820,  329.3153284734 },
    { 65.0472036707,  59.2848611429,  329.8488205655 },
    { 64.8269142223,  59.3247255635,  330.3878353270 },
    { 64.6056417117,  59.3694767690,  330.9324358037 },
    { 64.3833718843,  59.4192374826,  331.4826872759 },
    { 64.1600900911,  59.4741334088,  332.0386575075 },
    { 63.9357812708,  59.5342933201,  332.6004170221 },
    { 63.7104299318,  59.5998491482,  333.1680394094 },
    { 63.4840201321,  59.6709360816,  333.7416016641 },
    { 63.2565354584,  59.7476926680,  334.3211845627 },
    { 63.0279590038,  59.8302609234,  334.9068730799 },
    { 62.7982733434,  59.9187864484,  335.4987568514 },
    { 62.5674605089,  60.0134185520,  336.0969306872 },
    { 62.3355019609,  60.1143103851,  336.7014951409 },
    { 62.1023785592,  60.2216190824,  337.3125571428 },
    { 61.8680705312,  60.3355059167,  337.9302307033 },
    { 61.6325574371,  60.4561364641,  338.5546376957 },
    { 61.3958181329,  60.5836807848,  339.1859087283 },
    { 61.1578307305,  60.7183136174,  339.8241841174 },
    { 60.9185725537,  60.8602145933,  340.4696149737 },
    { 60.6780200911,  61.0095684693,  341.1223644177 },
    { 60.4361489444,  61.1665653844,  341.7826089417 },
    { 60.1929337720,  61.3314011430,  342.4505399390 },
    { 59.9483482273,  61.5042775278,  343.1263654236 },
    { 59.7023648910,  61.6854026486,  343.8103119690 },
    { 59.4549551966,  61.8749913317,  344.5026268995 },
    { 59.2060893477,  62.0732655566,  345.2035807722 },
    { 58.9557362272,  62.2804549489,  345.9134701972 },
    { 58.7038632952,  62.4967973390,  346.6326210509 },
    { 58.4504364763,  62.7225393981,  347.3613921486 },
    { 58.1954200323,  62.9579373683,  348.1001794579 },
    { 57.9387764197,  63.2032579023,  348.8494209488 },
    { 57.6804661283,  63.4587790388,  349.6096021994 },
    { 57.4204474979,  63.7247913407,  350.3812629029 },
    { 57.1586765095,  64.0015992317,  351.1650044543 },
    { 56.8951065457,  64.2895225791,  351.9614988398 },
    { 56.6296881133,  64.5888985809,  352.7714991071 },
    { 56.3623685228,  64.9000840335,  353.5958517679 },
    { 56.0930915119,  65.2234580790,  354.4355115775 },
    { 55.8217968036,  65.5594255639,  355.2915592654 },
    { 55.5484195809,  65.9084211811,  356.1652229548 },
    { 55.2728898580,  66.2709146299,  357.0579042423 },
    { 54.9951317209,  66.6474171093,  357.9712102164 },
    { 54.7150624006,  67.0384895803,  358.9069931327 },
    { 54.4325911295,  67.4447534047,  359.8674000721 }
};



const int gamutCuspTableSize = 360;//10;
const float limitJmax = 100.0; // equal to lin_to_Hellwig J
const float midJ = 34.0965; // ~10 nits in Hellwig J
const float cuspMidBlend = 1.3;
const float focusGainBlend = 0.2;
const float focusAdjustGain = 0.55;
const float focusDistance = 1.35;
const float focusDistanceScaling = 1.75;

const float n = 100.; // peak white
const float n_r = 100.; 
const float logPeak = log10( n / n_r);
const float focusDist = focusDistance + focusDistance * focusDistanceScaling * logPeak;

const float smoothCusps = 0.24;
const float smoothJ = 0.058;
const float smoothM = 0.188;

const float lowerHullGamma = 1.14;




float[3] viewingConditionsToSurround( int viewingConditions = 1) {
    float newSurround[3];

    // hack to turn incoming int value into surround coeffs
    if (viewingConditions == 0) {                 // "Dark"
        newSurround[0] = 0.8; newSurround[1] = 0.525; newSurround[2] = 0.8;
    } else if (viewingConditions == 1) {         // "Dim
        newSurround[0] = 0.9; newSurround[1] = 0.59; newSurround[2] = 0.9;
    } else if (viewingConditions == 2) {        // "Average"
        newSurround[0] = 1.0; newSurround[1] = 0.69; newSurround[2] = 1.0;
    } else {
            
    }
    return newSurround;
}

// Hellwig parameters
const float L_A = 100.0;    // luminance of adapting field
const float Y_b = 20.0;        // luminance of background

const float L_A_out = 100.0; 
const float Y_b_out = 20.0;

const float surround[3] = viewingConditionsToSurround( 1);
const float model_gamma = 1. / surround[1] * (1.48 + sqrt( Y_b_out / L_A_out ));
const float referenceLuminance = 100.0;

const float ac_resp = 1.0;
const float ra = 2. * ac_resp;
const float ba = 0.05 + (2. - ra);

float[3] add_f_f3( float a, float b[3])
{
  float out[3];
  out[0] = a + b[0];
  out[1] = a + b[1];
  out[2] = a + b[2];
  return out;
}

float[3][3] RGBtoXYZ_f33( Chromaticities C, float Y)
{
    // X and Z values of RGB value (1, 1, 1), or "white"
    float X = C.white[0] * Y / C.white[1];
    float Z = (1 - C.white[0] - C.white[1]) * Y / C.white[1];

    // Scale factors for matrix rows
    float d = C.red[0]   * (C.blue[1]  - C.green[1]) + 
        C.blue[0]  * (C.green[1] - C.red[1]) +
        C.green[0] * (C.red[1]   - C.blue[1]);

    float Sr = (X * (C.blue[1] - C.green[1]) -
        C.green[0] * (Y * (C.blue[1] - 1) +
        C.blue[1]  * (X + Z)) +
        C.blue[0]  * (Y * (C.green[1] - 1) +
        C.green[1] * (X + Z))) / d;

    float Sg = (X * (C.red[1] - C.blue[1]) +
        C.red[0]   * (Y * (C.blue[1] - 1) +
        C.blue[1]  * (X + Z)) -
        C.blue[0]  * (Y * (C.red[1] - 1) +
        C.red[1]   * (X + Z))) / d;

    float Sb = (X * (C.green[1] - C.red[1]) -
        C.red[0]   * (Y * (C.green[1] - 1) +
        C.green[1] * (X + Z)) +
        C.green[0] * (Y * (C.red[1] - 1) +
        C.red[1]   * (X + Z))) / d;

    // Assemble the matrix
    float M[3][3];

    M[0][0] = Sr * C.red[0];
    M[0][1] = Sr * C.red[1];
    M[0][2] = Sr * (1 - C.red[0] - C.red[1]);

    M[1][0] = Sg * C.green[0];
    M[1][1] = Sg * C.green[1];
    M[1][2] = Sg * (1 - C.green[0] - C.green[1]);

    M[2][0] = Sb * C.blue[0];
    M[2][1] = Sb * C.blue[1];
    M[2][2] = Sb * (1 - C.blue[0] - C.blue[1]);

    return M;
}

float[3][3] XYZtoRGB_f33(Chromaticities C, float Y)
{
    return invert_f33( RGBtoXYZ_f33(C, Y) );
}

float[3] HSV_to_RGB( float HSV[3] ) {
    float C = HSV[2] * HSV[1];
    float X = C * (1. - fabs( fmod(HSV[0] * 6., 2.) - 1.));
    float m = HSV[2] - C;
    
    float RGB[3];
    if (HSV[0] < 1./6.) {
        RGB[0] = C; RGB[1] = X; RGB[2] = 0.;
    } else if (HSV[0] < 2./6.) {
        RGB[0] = X; RGB[1] = C; RGB[2] = 0.;
    } else if (HSV[0] < 3./6.) {
        RGB[0] = 0.; RGB[1] = C; RGB[2] = X;
    } else if (HSV[0] < 4./6.) {
        RGB[0] = 0.; RGB[1] = X; RGB[2] = C;
    } else if (HSV[0] < 5./6.) {
        RGB[0] = X; RGB[1] = 0.; RGB[2] = C;
    } else {
        RGB[0] = C; RGB[1] = 0.; RGB[2] = X;
    }
    RGB = add_f_f3( m, RGB);
    return RGB;
}

const Chromaticities AP0 = // ACES Primaries from SMPTE ST2065-1
{
  { 0.73470,  0.26530},
  { 0.00000,  1.00000},
  { 0.00010, -0.07700},
  { 0.32168,  0.33767}
};

const Chromaticities AP1 = // Working space and rendering primaries for ACES 1.0
{
  { 0.713,    0.293},
  { 0.165,    0.830},
  { 0.128,    0.044},
  { 0.32168,  0.33767}
};

const Chromaticities REC709_PRI =
{
  { 0.64000,  0.33000},
  { 0.30000,  0.60000},
  { 0.15000,  0.06000},
  { 0.31270,  0.32900}
};

const Chromaticities INPUT_PRI = AP0;
const Chromaticities LIMIT_PRI = REC709_PRI;
const Chromaticities REACH_PRI = AP1;
const Chromaticities OUTPUT_PRI = REC709_PRI;

const float RGB_TO_XYZ_INPUT_MAT[3][3] = RGBtoXYZ_f33( INPUT_PRI, 1.0);
const float RGB_TO_XYZ_LIMIT_MAT[3][3] = RGBtoXYZ_f33( LIMIT_PRI, 1.0);
const float RGB_TO_XYZ_REACH_MAT[3][3] = RGBtoXYZ_f33( REACH_PRI, 1.0);
const float RGB_TO_XYZ_OUTPUT_MAT[3][3] = RGBtoXYZ_f33( OUTPUT_PRI, 1.0);

const float XYZ_TO_RGB_INPUT_MAT[3][3] = XYZtoRGB_f33( INPUT_PRI, 1.0);
const float XYZ_TO_RGB_LIMIT_MAT[3][3] = XYZtoRGB_f33( LIMIT_PRI, 1.0);
const float XYZ_TO_RGB_REACH_MAT[3][3] = XYZtoRGB_f33( REACH_PRI, 1.0);
const float XYZ_TO_RGB_OUTPUT_MAT[3][3] = XYZtoRGB_f33( OUTPUT_PRI, 1.0);

const float v[3] = {100., 100., 100.};
const float inWhite[3] = mult_f3_f33( v, RGB_TO_XYZ_INPUT_MAT);
const float outWhite[3] = mult_f3_f33( v, RGB_TO_XYZ_OUTPUT_MAT);
const float refWhite[3] = mult_f3_f33( v, RGB_TO_XYZ_LIMIT_MAT);

const Chromaticities CAM16_PRI = 
{
  { 0.8336,  0.1735 },
  { 2.3854, -1.4659 },
  { 0.13  , -0.14   },
  { 0.333 ,  0.333  }
};

const float MATRIX_16[3][3] = XYZtoRGB_f33( CAM16_PRI, 1.0);

float[3][3] generate_panlrcm() {
    float panlrcm_data[3][3] =
    {
        { ra,   1.    ,  1./9.},
        { 1., -12./11.,  1./9.},
        { ba,   1./11., -2./9.}
    };    
    float panlrcm[3][3] = invert_f33( panlrcm_data);

    // Normalize rows so that first column is 460
    for (int i = 0; i < 3; i = i + 1) {        
      float n = 460.0 / panlrcm[0][i];
      panlrcm[0][i] = n * panlrcm[0][i];
      panlrcm[1][i] = n * panlrcm[1][i];
      panlrcm[2][i] = n * panlrcm[2][i];
    }

    return panlrcm;
}
  
// Matrix for Hellwig inverse
const float panlrcm[3][3] = generate_panlrcm();


float radians_to_degrees( float radians)
{
    return radians * 180.0 / M_PI;
}

float degrees_to_radians( float degrees)
{
    return degrees / 180.0 * M_PI;
}

// safe divide function - return 0 if a divide by zero
float sdiv( float a, float b) {
    if (b == 0.0)
    {
        return 0.0;
    } else {
        return a / b;
    }
}

// linear interpolation between two values a & b with theß bias t
float lerp(float a, float b, float t)
{
    return a + t * (b - a);
}

void print_f3( float m[ 3])
{
   print( m[ 0], ",\t", m[ 1], ",\t", m[ 2], "\n");
}

float min( float a, float b)
{
  if (a < b)
    return a;
  else
    return b;
}

float max( float a, float b)
{
  if (a > b)
    return a;
  else
    return b;
}

int sign( float x)
{
    // Signum function:
    //  sign(X) returns 1 if the element is greater than zero, 0 if it equals zero 
    //  and -1 if it is less than zero

    int y;
    if (x < 0) { 
        y = -1;
    } else if (x > 0) {
        y = 1;
    } else {
        y = 0;
    }

    return y;
}

// safe power function to avoid NaNs or Infs when taking a fractional power of a negative base
// this initially returned -pow(abs(b), e) for negative b but this ended up producing 
// undesirable results in some cases, so now it just returns 0.0 instead
float spow( float base, float exponent) { 
    if ( base < 0.0 && exponent != floor(exponent) )
    {
        return 0.0;
    } else {
        return pow( base, exponent);
    }

}

// smooth minimum of a and b
float smin(float a, float b, float s)
{
    float h = max(s - fabs(a - b), 0.0) / s;
    return min(a, b) - h * h * h * s * (1.0 / 6.0);
}

float copysign( float x, float y)
{
    return sign(y) * fabs(x);
}

float wrap_to_360( float hue) {
    float y = fmod( hue, 360.);
    if ( y < 0.)
    {
        y = y + 360.;
    }
    return y;
}

float[3] post_adaptation_non_linear_response_compression_forward( float RGB[3], float F_L)
{
    float F_L_RGB[3];
    F_L_RGB[0] = spow(F_L * fabs(RGB[0]) / 100., 0.42);
    F_L_RGB[1] = spow(F_L * fabs(RGB[1]) / 100., 0.42);
    F_L_RGB[2] = spow(F_L * fabs(RGB[2]) / 100., 0.42);
    
    float RGB_c[3];
    RGB_c[0] = (400. * copysign(1., RGB[0]) * F_L_RGB[0]) / (27.13 + F_L_RGB[0]);
    RGB_c[1] = (400. * copysign(1., RGB[1]) * F_L_RGB[1]) / (27.13 + F_L_RGB[1]);
    RGB_c[2] = (400. * copysign(1., RGB[2]) * F_L_RGB[2]) / (27.13 + F_L_RGB[2]);

    return RGB_c;
}

float[3] post_adaptation_non_linear_response_compression_inverse( float RGB[3], float F_L)
{
    float RGB_p[3];
    RGB_p[0] = (sign(RGB[0]) * 100. / F_L * spow((27.13 * fabs(RGB[0])) / (400. - fabs(RGB[0])), 1.0 / 0.42));
    RGB_p[1] = (sign(RGB[1]) * 100. / F_L * spow((27.13 * fabs(RGB[1])) / (400. - fabs(RGB[1])), 1.0 / 0.42));
    RGB_p[2] = (sign(RGB[2]) * 100. / F_L * spow((27.13 * fabs(RGB[2])) / (400. - fabs(RGB[2])), 1.0 / 0.42));

    return RGB_p;
}

float base_hue_for_position(int i_lo, int table_size) {
    float result = i_lo * 360. / table_size;
    return result;
}

float[3] Hellwig2022_JMh_to_XYZ( float JMh[3], float XYZ_w[3], float L_A, float Y_b) 
{
    float J = JMh[0];
    float M = JMh[1];
    float h = JMh[2];
    
    float Y_w = XYZ_w[1];
    
    // Step 0 - Converting CIE XYZ tristimulus values to sharpened RGB values.
    float RGB_w[3] = mult_f3_f33( XYZ_w, MATRIX_16);

    // Ignore degree of adaptation.
    float D = 1.0;
    
    // Viewing condition dependent parameters
    float k = 1.0 / (5.0 * L_A + 1.0);
    float k4 = pow( k, 4.0);
    float F_L = 0.2 * k4 * (5.0 * L_A) + 0.1 * pow( (1.0-k4), 2.0) * spow( 5.0 * L_A, 1.0/3.0);
    float n = sdiv( Y_b, Y_w);
    float z = 1.48 + sqrt(n);

    float D_RGB[3];
    D_RGB[0] = D * Y_w / RGB_w[0] + 1.0 - D;
    D_RGB[1] = D * Y_w / RGB_w[1] + 1.0 - D;
    D_RGB[2] = D * Y_w / RGB_w[2] + 1.0 - D;
    
    float RGB_wc[3];
    RGB_wc[0] = D_RGB[0] * RGB_w[0];
    RGB_wc[1] = D_RGB[1] * RGB_w[1];
    RGB_wc[2] = D_RGB[2] * RGB_w[2];

    // Apply forward post-adaptation non-linear response compression
    float F_L_RGB[3];
    F_L_RGB[0] = spow( F_L * fabs(RGB_wc[0]) / 100.0, 0.42);
    F_L_RGB[1] = spow( F_L * fabs(RGB_wc[1]) / 100.0, 0.42);
    F_L_RGB[2] = spow( F_L * fabs(RGB_wc[2]) / 100.0, 0.42);

    // Computing achromatic responses for the whitepoint
    float R_aw = (400.0 * sign(RGB_wc[0]) * F_L_RGB[0]) / (27.13 + F_L_RGB[0]);
    float G_aw = (400.0 * sign(RGB_wc[1]) * F_L_RGB[1]) / (27.13 + F_L_RGB[1]);
    float B_aw = (400.0 * sign(RGB_wc[2]) * F_L_RGB[2]) / (27.13 + F_L_RGB[2]);
    float A_w = ra * R_aw + G_aw + ba * B_aw;

    float hr = degrees_to_radians(h);
    
    // No Helmholtz-Kohlrausch effect
    
    // Computing achromatic respons A for the stimulus
    float A = A_w * spow(J/100.0, 1.0/ (surround[1] * z));
    
    // Computing P_p_1 to P_p_2
    float P_p_1 = 43.0 * surround[2];
    float P_p_2 = A;
    
    // Step 3 - Computing opponent colour dimensions a and b
    float gamma = M / P_p_1;
    float a = gamma * cos(hr);
    float b = gamma * sin(hr);
    
    // Step 4 - Applying post-adaptation non-linear response compression matrix
    float vec[3] = {P_p_2, a, b};
    float RGB_a[3] = mult_f_f3( 1.0/1403.0, mult_f3_f33( vec, panlrcm));

    // Step 5 - Applying the inverse post-adaptation non-linear respnose compression
    float RGB_c[3] = post_adaptation_non_linear_response_compression_inverse( RGB_a, F_L);

    // Step 6
    float RGB[3];
    RGB[0] = RGB_c[0] / D_RGB[0];
    RGB[1] = RGB_c[1] / D_RGB[1];
    RGB[2] = RGB_c[2] / D_RGB[2];
    
    // Step 7
    float XYZ[3] = mult_f3_f33( RGB, invert_f33( MATRIX_16) );
    
    return XYZ;
}

float[3] XYZ_to_Hellwig2022_JMh( float XYZ[3], float XYZ_w[3], float L_A, float Y_b)
{
    float Y_w = XYZ_w[1];

    // Step 0 - Converting CIE XYZ tristimulus values to sharpened RGB values
    float RGB_w[3] = mult_f3_f33( XYZ_w, MATRIX_16);
     
    // Ignore degree of adaptation.
    float D = 1.0;
    
    // Viewing condition dependent parameters
    float k = 1. / (5. * L_A + 1.);
    float k4 = pow( k, 4.);
    float F_L = 0.2 * k4 * (5. * L_A) + 0.1 * pow( (1.-k4), 2.) * spow( 5. * L_A, 1./3.);
    float n = Y_b / Y_w;
    float z = 1.48 + sqrt(n);

    float D_RGB[3];
    D_RGB[0] = D * Y_w / RGB_w[0] + 1. - D;
    D_RGB[1] = D * Y_w / RGB_w[1] + 1. - D;
    D_RGB[2] = D * Y_w / RGB_w[2] + 1. - D;
    
    float RGB_wc[3];
    RGB_wc[0] = D_RGB[0] * RGB_w[0];
    RGB_wc[1] = D_RGB[1] * RGB_w[1];
    RGB_wc[2] = D_RGB[2] * RGB_w[2];

    float RGB_aw[3] = post_adaptation_non_linear_response_compression_forward( RGB_wc, F_L);    
    float A_w = ra * RGB_aw[0] + RGB_aw[1] + ba * RGB_aw[2];
    
    // Step 1 - Converting CIE XYZ tristimulus values to sharpened RGB values
    float RGB[3] = mult_f3_f33( XYZ, MATRIX_16);

    // Step 2
    float RGB_c[3];
    RGB_c[0] = D_RGB[0] * RGB[0];
    RGB_c[1] = D_RGB[1] * RGB[1];
    RGB_c[2] = D_RGB[2] * RGB[2];
    
    // Step 3 - apply  forward post-adaptation non-linear response compression
    float RGB_a[3] = post_adaptation_non_linear_response_compression_forward( RGB_c, F_L);

    // Step 4 - Converting to preliminary cartesian  coordinates
    float a = RGB_a[0] - 12. * RGB_a[1] / 11. + RGB_a[2] / 11.;
    float b = (RGB_a[0] + RGB_a[1] - 2. * RGB_a[2]) / 9.;

    // Computing the hue angle math h
    // Unclear why this isn't matching the python version
    float hr = atan2(b,a);
    float h = wrap_to_360( radians_to_degrees(hr) );
    
    // Step 6 - Computing achromatic responses for the stimulus
    float A = ra * RGB_a[0] + RGB_a[1] + ba * RGB_a[2];

    // Step 7 - Computing the correlate of lightness, J
    float J = 100. * pow( A / A_w, surround[1] * z);
    
    // Step 9 - Computing the correlate of colourfulness, M
    float M = 43. * surround[2] * sqrt(a * a + b * b);
    
    if (J == 0.0)
        M = 0.0;

    float return_JMh[3] = {J, M, h};
    return return_JMh;
}


float[3] RGB_to_JMh( float RGB[3], float matrix[3][3]) {
    float luminanceRGB[3] = mult_f_f3( n, RGB );
    float XYZ[3] = mult_f3_f33( luminanceRGB, matrix );
    float JMh[3] = XYZ_to_Hellwig2022_JMh( XYZ, refWhite, L_A, Y_b );
    return JMh;
}


float[3] JMh_to_RGB( float JMh[3], float matrix[3][3]) {
    float luminanceXYZ[3] = Hellwig2022_JMh_to_XYZ( JMh, refWhite, L_A, Y_b);
    float luminanceRGB[3] = mult_f3_f33( luminanceXYZ, matrix);
    float RGB[3] = mult_f_f3( 1./n, luminanceRGB);
    return RGB;
}

float solve_J_intersect(
    float J,
    float M,
    float focusJ, 
    float maxJ, 
    float slope_gain
)
{
    float a = M / (focusJ * slope_gain);
    float b = 0.0;
    float c = 0.0;
    float intersectJ = 0.0;
    
    if (J < focusJ) {
        b = 1.0 - M / slope_gain;
    } else {
        b= -(1.0 + M / slope_gain + maxJ * M / (focusJ * slope_gain));
    } 

    if (J < focusJ) {
        c = -J;
    } else {
        c = maxJ * M / slope_gain + J;
    }

    float root = sqrt( b * b - 4.0 * a * c);

    if (J < focusJ){
        intersectJ = 2.0 * c / (-b - root);
    } else {
        intersectJ = 2.0 * c / (-b + root);
    } 

    return intersectJ;
}


// reimplemented from https://github.com/nick-shaw/aces-ot-vwg-experiments/blob/master/python/intersection_approx.py
float[3] findGamutBoundaryIntersection( 
    float JMh_s[3], 
    float JM_cusp_in[2], 
    float J_focus, 
    float J_max, 
    float slope_gain, 
    float smoothness,
    float gamma_top,
    float gamma_bottom
)
{
    float slope = 0.0;

    float s = max(0.000001, smoothness);
    float JM_cusp[2];
    JM_cusp[0] = JM_cusp_in[0] * (1.0 + smoothJ * s);   // J
    JM_cusp[1] = JM_cusp_in[1] * (1.0 + smoothM * s);   // M

    float J_intersect_source = solve_J_intersect( JMh_s[0], JMh_s[1], J_focus, J_max, slope_gain);
    float J_intersect_cusp = solve_J_intersect( JM_cusp[0], JM_cusp[1], J_focus, J_max, slope_gain);

    if (J_intersect_source < J_focus) {
        slope = J_intersect_source * (J_intersect_source - J_focus) / (J_focus * slope_gain);
    } else {
        slope = (J_max - J_intersect_source) * (J_intersect_source - J_focus) / (J_focus * slope_gain);
    } 

    float M_boundary_lower = J_intersect_cusp * pow(J_intersect_source / J_intersect_cusp, 1. / gamma_bottom) / (JM_cusp[0] / JM_cusp[1] - slope);

    float M_boundary_upper = JM_cusp[1] * (J_max - J_intersect_cusp) * pow((J_max - J_intersect_source) / (J_max - J_intersect_cusp), 1. / gamma_top) / (slope * JM_cusp[1] + J_max - JM_cusp[0]);

    float M_boundary = JM_cusp[1] * smin(M_boundary_lower / JM_cusp[1], M_boundary_upper / JM_cusp[1], s);

    float J_boundary = J_intersect_source + slope * M_boundary;

    float return_JMh[3] = { J_boundary, M_boundary, J_intersect_source };

    return return_JMh;
}

// https://www.desmos.com/calculator/oe2fscya80
float getFocusGain(float J, float cuspJ) {

    float thr = lerp(cuspJ, limitJmax, focusGainBlend);
    if (J > thr) {
        // Approximate inverse required above threshold
        float gain = (limitJmax - thr) / max(0.0001, (limitJmax - min(limitJmax, J)));
        return pow( log10(gain), 1. / focusAdjustGain) + 1.;
    } else {
        // Analytic inverse possible below cusp
        return 1.;
    }
}

float[2] cuspFromTable( float h, float table[][3])
{
    float lo[3];
    float hi[3];
    
    if (h <= table[0][2]) {
        lo = table[gamutCuspTableSize-1];
        lo[2] = lo[2]-360.0;
        hi = table[0];
    } else if (h >= table[gamutCuspTableSize-1][2]) {
        lo = table[gamutCuspTableSize-1];
        hi = table[0];
        hi[2] = hi[2]+360.0;
    } else {
        for (int i = 1; i < gamutCuspTableSize; i = i+1) {
            if (h <= table[i][2] && h > table[i-1][2]) {
                lo = table[i-1];
                hi = table[i];
            }
        }
    }
    
    float t = (h - lo[2]) / (hi[2] - lo[2]);
    float cuspJ = lerp( lo[0], hi[0], t);
    float cuspM = lerp( lo[1], hi[1], t);
    
    float cuspJM[2] = { cuspJ, cuspM };
    
    return cuspJM;    
}


bool any_below_zero( float newLimitRGB[3]) 
{
    if (newLimitRGB[0] < 0. || newLimitRGB[1] < 0. || newLimitRGB[2] < 0.) {
        return true;
    } 
    return false;
}

float[gamutCuspTableSize][3] make_gamut_table( float RGB_TO_XYZ_LIMIT_MAT[3][3] ) {
    float gamutCuspTableUnsorted[gamutCuspTableSize][3];
    
    int i;
    for (i = 0; i < gamutCuspTableSize; i=i+1) {
        float i_float = i;
        float hNorm = i_float / gamutCuspTableUnsorted.size;
    
        float HSV[3] = {hNorm, 1., 1.};
        float RGB[3] = HSV_to_RGB( HSV); 

//         print_f3( HSV); //print("\n");
//         print( "\nRGB\n"); print_f3( RGB);
    
        gamutCuspTableUnsorted[i] = RGB_to_JMh( RGB, RGB_TO_XYZ_LIMIT_MAT);
//         print_f3( gamutCuspTableUnsorted[i]); 

    }

    int minhIndex = 0;
    for (i = 0; i < gamutCuspTableSize; i=i+1) {
        if ( gamutCuspTableUnsorted[i][2] < gamutCuspTableUnsorted[minhIndex][2])
            minhIndex = i;
    }

    float gamutCuspTable[gamutCuspTableSize][3];
    for (i = 0; i < gamutCuspTableSize; i=i+1) {
        gamutCuspTable[i] = gamutCuspTableUnsorted[(minhIndex+i) % gamutCuspTableSize];
        print_f3( gamutCuspTable[i] );
    }

    return gamutCuspTable;
}


float[gamutCuspTableSize] make_gamut_reach_table( float XYZ_TO_RGB_REACH_MAT[3][3] ) {

    float gamutReachTable[gamutCuspTableSize];

    int i;
    for (i = 0; i < gamutCuspTableSize; i=i+1) {
        float i_float = i;
        float hue = base_hue_for_position( i, gamutCuspTableSize);

        float search_range = 50.;
        float low = 0.;
        float high = low + search_range;
        bool outside = false;

        while ((outside != true) & (high < 1300.))
        {
            float searchJMh[3] = { limitJmax, high, hue };
            float newLimitRGB[3] = JMh_to_RGB( searchJMh, XYZ_TO_RGB_REACH_MAT );
            outside = any_below_zero( newLimitRGB);
            if (outside == false) {
                low = high;
                high = high + search_range;
            }
        }

        while (high-low > 1e-3) 
        {
            float sampleM = (high + low) / 2.;
            float searchJMh[3] = { limitJmax, sampleM, hue };
            float newLimitRGB[3] = JMh_to_RGB( searchJMh, XYZ_TO_RGB_REACH_MAT );
            outside = any_below_zero( newLimitRGB);
            if (outside) {
                high = sampleM;
            } else {
                low = sampleM;
            }
        }
        
        gamutReachTable[i] = low;
//         print( gamutReachTable[i], "\n" );

//         gamutReachTable[i][0] = low;
//         gamutReachTable[i][1] = high;
//         gamutReachTable[i][2] = hue;

//         print( "\nhue:\t", hue, "\n");
        print( gamutReachTable[i], "\n" );
    }
    return gamutReachTable;
    
}

bool outside_hull( float newLimitRGB[3] )
{
    // limit value, once we cross this value, we are outside of the top gamut shell
    float maxRGBtestVal = 1.0;
    if (newLimitRGB[0] > maxRGBtestVal || newLimitRGB[1] > maxRGBtestVal || newLimitRGB[2] > maxRGBtestVal)
    {
        return true;
    }
    return false;
}


bool evaluate_gamma_fit( float JMcusp[2], 
                                    float testJMh[][3], 
                                    int test_count, 
                                    float topGamma, 
                                    bool debug )
{
    float focusJ = lerp(JMcusp[0], midJ, min(1.0, cuspMidBlend - (JMcusp[0] / limitJmax)));
    for (int testIndex = 0; testIndex < test_count; testIndex = testIndex + 1)
    {
        float slope_gain = limitJmax * focusDist * getFocusGain( testJMh[testIndex][0], JMcusp[0]);
        float approxLimit[3] = findGamutBoundaryIntersection( testJMh[testIndex], JMcusp, focusJ, limitJmax, slope_gain, smoothCusps, topGamma, lowerHullGamma);
        float approximate_JMh[3] = { approxLimit[0], approxLimit[1], testJMh[testIndex][2] };
        float newLimitRGB[3] = JMh_to_RGB( approximate_JMh, XYZ_TO_RGB_LIMIT_MAT);

        if (debug) {
        print( "\ntopGamma\t", topGamma);
        print( "\napproxLimit\t"); print_f3( approxLimit);
        print( "newLimitRGB\t"); print_f3( newLimitRGB);
        }

        if (!outside_hull(newLimitRGB))
        {
            return false;
        }
    }
    return true;
}


float[gamutCuspTableSize] make_upper_hull_gamma( float gamutCuspTable[][3] ) {
    // Find upper hull gamma values for the gamut mapper
    // Start by taking a h angle
    // Get the cusp J value for that angle
    // Find a J value halfway to the Jmax
    // Iterate through gamma values until the approximate max M is negative through the actual boundary

    // positions between the cusp and Jmax we will check
    // variables that get set as we iterate through, once all are set to true we break the loop

    const int test_count = 3;
    float testPositions[test_count] = {0.01, 0.5, 0.99};
    float gamutTopGamma[gamutCuspTableSize];

    for (int i = 0; i < gamutCuspTableSize; i = i + 1)
    {
        // default value. This will get overridden as we loop, but can be a good diagnostic 
        // to make sure things are working
        gamutTopGamma[i] = -1.;
        
        // get cusp from cusp table at hue position
        float hue = base_hue_for_position(i, gamutCuspTableSize);
        float JMcusp[2] = cuspFromTable( hue, gamutCuspTable);
        
        float testJMh[test_count][3];
        // create test values halfway between the cusp and the Jmax
        for (int testIndex = 0; testIndex < test_count; testIndex = testIndex + 1) {
            float testJ = JMcusp[0] + ((limitJmax - JMcusp[0]) * testPositions[testIndex]);
//             print( "\ntestJMh\t", testJ, "\t", JMcusp[1], "\t", hue);
            testJMh[testIndex][0] = testJ;
            testJMh[testIndex][1] = JMcusp[1];
            testJMh[testIndex][2] = hue;
        }

        float search_range = 0.4;
        float low = 0.;
        float high = low + search_range;
        bool outside = false;
        
        while (!(outside) && (high < 5.0)) 
        {
            bool gammaFound = evaluate_gamma_fit( JMcusp, testJMh, test_count, high, false);
            if (!gammaFound)
            {
                low = high;
                high = high + search_range;
            } else {
                outside = true;
            }
        }
        
        float testGamma = -1.0;
        while ( (high-low) > 1e-5)
        {
            testGamma = (high + low) / 2.;
            bool gammaFound = evaluate_gamma_fit( JMcusp, testJMh, test_count, testGamma, false);
            if (gammaFound)
            {
                high = testGamma;
                gamutTopGamma[i] = high;
            } else {
                low = testGamma;
            }
        }

        if (gamutTopGamma[i] < 0.) {
            print( "Did not find top gamma for hue: ", hue, "\n");
        }
        print( "hue " , hue, ":\t\t",gamutTopGamma[i], "\n");
    }

    return gamutTopGamma;
}





void main ( 
    input varying float rIn,
    input varying float gIn,
    input varying float bIn,
    output varying float rOut,
    output varying float gOut,
    output varying float bOut,
    output varying float aOut,
    input varying float aIn = 1.
) 
{
    // Put input variables into a 3-element array (ACES)
    float aces[3] = {rIn, gIn, bIn};
    
//     print( "\nXYZ_TO_RGB_INPUT_MAT:\n"); print_f33(XYZ_TO_RGB_INPUT_MAT);
//     print( "\nXYZ_TO_RGB_LIMIT_MAT:\n"); print_f33(XYZ_TO_RGB_LIMIT_MAT);
//     print( "\nXYZ_TO_RGB_REACH_MAT:\n"); print_f33(XYZ_TO_RGB_REACH_MAT);
//     print( "\nXYZ_TO_RGB_OUTPUT_MAT:\n"); print_f33(XYZ_TO_RGB_OUTPUT_MAT);
// 
//     print( "\nRGB_TO_XYZ_LIMIT_MAT:\n"); print_f33(RGB_TO_XYZ_LIMIT_MAT);
//     print( "\nRGB_TO_XYZ_LIMIT_MAT:\n"); print_f33( invert_f33(XYZ_TO_RGB_LIMIT_MAT));

//     print( "\nCAM16_MAT:\n"); print_f33( MATRIX_16);
//     print( "\npanlrcm_MAT:\n"); print_f33( panlrcm);

    // Tables for chroma compression (same for all transforms?)
    print("\ngamutCuspTable:\n");
    const float cgamutCuspTable[gamutCuspTableSize][3] = make_gamut_table( RGB_TO_XYZ_REACH_MAT);

    print("\ngamutReachTable:\n");
    const float cgamutReachTable[gamutCuspTableSize] = make_gamut_reach_table( XYZ_TO_RGB_REACH_MAT);
    
    // Tables for limiting gamut
    print("\ngamutCuspTable:\n");
    const float gamutCuspTable[gamutCuspTableSize][3] = make_gamut_table( RGB_TO_XYZ_LIMIT_MAT);
    
    print("\nupperHullGamma:\n");
    const float gamutTopGamma[gamutCuspTableSize] = make_upper_hull_gamma( gamutCuspTable );

    rOut = aces[0];
    gOut = aces[1];
    bOut = aces[2];
    aOut = aIn;

}